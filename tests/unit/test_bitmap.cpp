// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Phase 1 acceptance tests. Written against docs/roadmap.md and
// docs/bitmap-history.md before the implementation existed.
//
// Invariants under test, in priority order:
//   1. A write touching one byte of a region dirties the whole region.
//   2. No bit outside the device is ever set (tail masking).
//   3. Invalid input changes nothing (all-or-nothing).
//   4. Overflow is reported, never executed.

#include <cstdint>
#include <limits>
#include <ostream>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "blockchronicle/bitmap.h"

namespace blockchronicle {

// Lets EXPECT_EQ print a readable status instead of an integer.
std::ostream& operator<<(std::ostream& os, Status status)
{
    return os << toString(status);
}

} // namespace blockchronicle

namespace {

using namespace blockchronicle;

constexpr std::uint64_t kKiB = 1024ull;
constexpr std::uint64_t kMiB = 1024ull * kKiB;
constexpr std::uint32_t k64K = 64u * 1024u;

Geometry makeGeometry(std::uint64_t capacityBytes, std::uint32_t granularity = k64K)
{
    Geometry geometry;
    const Status status = Geometry::make(capacityBytes, granularity, geometry);
    EXPECT_EQ(status, Status::Ok);
    return geometry;
}

std::vector<std::uint32_t> dirtyRegions(const DirtyBitmap& bitmap)
{
    std::vector<std::uint32_t> regions;
    for (std::uint32_t region = 0; region < bitmap.regionCount(); ++region) {
        if (bitmap.test(region)) {
            regions.push_back(region);
        }
    }
    return regions;
}

class RegionCollector : public RangeVisitor {
public:
    void onRegionRange(std::uint32_t firstRegion, std::uint32_t lastRegion) override
    {
        spans.push_back(RegionSpan{firstRegion, lastRegion});
    }

    std::vector<RegionSpan> spans;
};

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

TEST(Geometry, DerivesRegionCountFromCapacity)
{
    EXPECT_EQ(makeGeometry(1 * kMiB).regionCount(), 16u);
    EXPECT_EQ(makeGeometry(1 * kMiB + 1).regionCount(), 17u);
    EXPECT_EQ(makeGeometry(1).regionCount(), 1u);
    EXPECT_EQ(makeGeometry(16 * k64K).regionCount(), 16u);
}

TEST(Geometry, SupportsNonPowerOfTwoGranularity)
{
    const Geometry geometry = makeGeometry(2500, 1000);
    EXPECT_EQ(geometry.regionCount(), 3u);
    EXPECT_EQ(geometry.regionLengthBytes(0), 1000u);
    EXPECT_EQ(geometry.regionLengthBytes(2), 500u);
}

TEST(Geometry, RejectsDegenerateInput)
{
    Geometry geometry;
    EXPECT_EQ(Geometry::make(0, k64K, geometry), Status::InvalidArgument);
    EXPECT_EQ(Geometry::make(1 * kMiB, 0, geometry), Status::InvalidArgument);
}

TEST(Geometry, RejectsRegionCountThatDoesNotFitIn32Bits)
{
    Geometry geometry;
    // 2^32 regions at 1 byte each exceeds the 32-bit region index space.
    EXPECT_EQ(Geometry::make((1ull << 32) + 1, 1, geometry), Status::OutOfRange);
}

TEST(Geometry, FinalRegionIsClampedToCapacity)
{
    const Geometry geometry = makeGeometry(16 * k64K + 1000);
    EXPECT_EQ(geometry.regionCount(), 17u);
    EXPECT_EQ(geometry.regionOffsetBytes(16), 16 * k64K);
    EXPECT_EQ(geometry.regionLengthBytes(15), k64K);
    EXPECT_EQ(geometry.regionLengthBytes(16), 1000u);
}

// ---------------------------------------------------------------------------
// Marking
// ---------------------------------------------------------------------------

TEST(DirtyBitmap, MarkUnalignedCrossBoundaryMarksBothRegions)
{
    // The acceptance test from the project brief:
    // 1 MiB device, granularity 64 KiB, write at 60000 length 10000.
    DirtyBitmap bitmap(makeGeometry(1 * kMiB));

    ASSERT_EQ(bitmap.markWrite(60000, 10000), Status::Ok);

    EXPECT_TRUE(bitmap.test(0));
    EXPECT_TRUE(bitmap.test(1));
    EXPECT_FALSE(bitmap.test(2));
    EXPECT_EQ(bitmap.countDirty(), 2u);
    EXPECT_EQ(dirtyRegions(bitmap), (std::vector<std::uint32_t>{0, 1}));
}

TEST(DirtyBitmap, MarkZeroLengthIsOkAndMarksNothing)
{
    DirtyBitmap bitmap(makeGeometry(1 * kMiB));

    EXPECT_EQ(bitmap.markWrite(12345, 0), Status::Ok);
    EXPECT_EQ(bitmap.markWrite(1 * kMiB, 0), Status::Ok);
    EXPECT_TRUE(bitmap.none());
    EXPECT_EQ(bitmap.countDirty(), 0u);
}

TEST(DirtyBitmap, MarkAtEndOfDeviceMarksOnlyTheLastRegion)
{
    DirtyBitmap bitmap(makeGeometry(1 * kMiB));

    ASSERT_EQ(bitmap.markWrite(1 * kMiB - 1, 1), Status::Ok);

    EXPECT_EQ(dirtyRegions(bitmap), (std::vector<std::uint32_t>{15}));
}

TEST(DirtyBitmap, MarkPastEndIsAllOrNothing)
{
    DirtyBitmap bitmap(makeGeometry(1 * kMiB));

    EXPECT_EQ(bitmap.markWrite(1 * kMiB - 10, 100), Status::OutOfRange);
    EXPECT_TRUE(bitmap.none()) << "a rejected write must not mark the regions it did cover";
}

TEST(DirtyBitmap, MarkBeginningBeyondCapacityIsRejected)
{
    DirtyBitmap bitmap(makeGeometry(1 * kMiB));

    EXPECT_EQ(bitmap.markWrite(16 * kMiB, 1), Status::OutOfRange);
    EXPECT_TRUE(bitmap.none());
}

TEST(DirtyBitmap, MarkOverflowIsReportedNotExecuted)
{
    DirtyBitmap bitmap(makeGeometry(1 * kMiB));

    // offset + length overflows uint64. Must be rejected without UB (UBSan runs
    // this test in CI).
    EXPECT_EQ(bitmap.markWrite(std::numeric_limits<std::uint64_t>::max() - 7, 16),
              Status::OutOfRange);
    EXPECT_EQ(bitmap.markWrite(0, std::numeric_limits<std::uint64_t>::max()),
              Status::OutOfRange);
    EXPECT_EQ(bitmap.markWrite(std::numeric_limits<std::uint64_t>::max(), 1),
              Status::OutOfRange);
    EXPECT_TRUE(bitmap.none());
}

TEST(DirtyBitmap, DuplicateMarksAreIdempotent)
{
    DirtyBitmap bitmap(makeGeometry(1 * kMiB));

    for (int i = 0; i < 1000; ++i) {
        ASSERT_EQ(bitmap.markWrite(70 * kKiB, 1), Status::Ok);
    }

    EXPECT_EQ(bitmap.countDirty(), 1u);
    EXPECT_EQ(dirtyRegions(bitmap), (std::vector<std::uint32_t>{1}));
}

TEST(DirtyBitmap, FullDeviceWriteMarksEveryRegionExactlyOnce)
{
    DirtyBitmap bitmap(makeGeometry(1 * kMiB));

    ASSERT_EQ(bitmap.markWrite(0, 1 * kMiB), Status::Ok);

    EXPECT_EQ(bitmap.countDirty(), 16u);
    EXPECT_EQ(bitmap.dirtyBytes(), 1 * kMiB);

    RegionCollector collector;
    bitmap.forEachRange(collector);
    ASSERT_EQ(collector.spans.size(), 1u);
    EXPECT_EQ(collector.spans[0].firstRegion, 0u);
    EXPECT_EQ(collector.spans[0].lastRegion, 15u);
}

TEST(DirtyBitmap, MarkRegionRejectsOutOfRange)
{
    DirtyBitmap bitmap(makeGeometry(1 * kMiB));

    EXPECT_FALSE(bitmap.markRegion(16));
    EXPECT_FALSE(bitmap.markRegionRange(15, 16));
    EXPECT_FALSE(bitmap.markRegionRange(5, 4));
    EXPECT_TRUE(bitmap.none());
}

// ---------------------------------------------------------------------------
// Tail handling
// ---------------------------------------------------------------------------

TEST(DirtyBitmap, TailBitsAreNeverSet)
{
    // 17 regions: one word with 47 bits spare.
    DirtyBitmap bitmap(makeGeometry(1 * kMiB + 1));

    // The last byte of the device is in region 16, the partially tracked one.
    ASSERT_EQ(bitmap.markWrite(1 * kMiB, 1), Status::Ok);

    EXPECT_EQ(bitmap.wordCount(), 1u);
    EXPECT_EQ(bitmap.words()[0], (Word{1} << 16));
    EXPECT_EQ(bitmap.countDirty(), 1u);
}

TEST(DirtyBitmap, BitwiseOperationsNeverSetTailBits)
{
    DirtyBitmap a(makeGeometry(1 * kMiB + 1));
    DirtyBitmap b(makeGeometry(1 * kMiB + 1));
    ASSERT_EQ(a.markWrite(16 * k64K, 1), Status::Ok);

    DirtyBitmap result(makeGeometry(1 * kMiB + 1));

    ASSERT_EQ(DirtyBitmap::bitwiseOr(a, b, result), Status::Ok);
    EXPECT_EQ(result.words()[0], (Word{1} << 16));

    ASSERT_EQ(DirtyBitmap::bitwiseAndNot(a, b, result), Status::Ok);
    EXPECT_EQ(result.words()[0], (Word{1} << 16));

    ASSERT_EQ(DirtyBitmap::bitwiseXor(a, b, result), Status::Ok);
    EXPECT_EQ(result.words()[0], (Word{1} << 16));
}

// ---------------------------------------------------------------------------
// Range enumeration
// ---------------------------------------------------------------------------

TEST(DirtyBitmap, CoalescesAdjacentRegionsIntoByteRanges)
{
    DirtyBitmap bitmap(makeGeometry(64 * k64K));

    for (std::uint32_t region : {10u, 11u, 12u, 13u, 20u, 21u, 50u}) {
        ASSERT_TRUE(bitmap.markRegion(region));
    }

    std::vector<DirtyRange> ranges;
    ASSERT_EQ(bitmap.toRanges(16, ranges), Status::Ok);
    ASSERT_EQ(ranges.size(), 3u);

    EXPECT_EQ(ranges[0].offsetBytes, 10 * k64K);
    EXPECT_EQ(ranges[0].lengthBytes, 4 * k64K);

    EXPECT_EQ(ranges[1].offsetBytes, 20 * k64K);
    EXPECT_EQ(ranges[1].lengthBytes, 2 * k64K);

    EXPECT_EQ(ranges[2].offsetBytes, 50 * k64K);
    EXPECT_EQ(ranges[2].lengthBytes, 1 * k64K);

    RegionCollector collector;
    bitmap.forEachRange(collector);
    ASSERT_EQ(collector.spans.size(), 3u);
    EXPECT_EQ(collector.spans[0].firstRegion, 10u);
    EXPECT_EQ(collector.spans[0].lastRegion, 13u);
    EXPECT_EQ(collector.spans[2].firstRegion, 50u);
    EXPECT_EQ(collector.spans[2].lastRegion, 50u);
}

TEST(DirtyBitmap, FinalPartialRegionLengthIsClampedToCapacity)
{
    DirtyBitmap bitmap(makeGeometry(16 * k64K + 1000));
    ASSERT_TRUE(bitmap.markRegion(16));

    std::vector<DirtyRange> ranges;
    ASSERT_EQ(bitmap.toRanges(4, ranges), Status::Ok);
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].offsetBytes, 16 * k64K);
    EXPECT_EQ(ranges[0].lengthBytes, 1000u);
    EXPECT_EQ(bitmap.dirtyBytes(), 1000u);
}

TEST(DirtyBitmap, ToRangesExceedingLimitFailsWithoutPartialOutput)
{
    DirtyBitmap bitmap(makeGeometry(64 * k64K));
    for (std::uint32_t region : {1u, 3u, 5u, 7u, 9u}) {
        ASSERT_TRUE(bitmap.markRegion(region));
    }

    std::vector<DirtyRange> ranges(1, DirtyRange{42, 42});
    EXPECT_EQ(bitmap.toRanges(4, ranges), Status::OutOfRange);
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].offsetBytes, 42u) << "out must be untouched on failure";
}

TEST(DirtyBitmap, EmptyBitmapHasNoRanges)
{
    DirtyBitmap bitmap(makeGeometry(1 * kMiB));

    std::vector<DirtyRange> ranges;
    EXPECT_EQ(bitmap.toRanges(1, ranges), Status::Ok);
    EXPECT_TRUE(ranges.empty());

    RegionCollector collector;
    bitmap.forEachRange(collector);
    EXPECT_TRUE(collector.spans.empty());
}

// ---------------------------------------------------------------------------
// Bitwise operations
// ---------------------------------------------------------------------------

TEST(DirtyBitmap, BitwiseMatchesTheBriefExample)
{
    // A = {10,20,30}, B = {20,30,40,50}
    const Geometry geometry = makeGeometry(64 * k64K);
    DirtyBitmap a(geometry);
    DirtyBitmap b(geometry);
    for (std::uint32_t region : {10u, 20u, 30u}) {
        ASSERT_TRUE(a.markRegion(region));
    }
    for (std::uint32_t region : {20u, 30u, 40u, 50u}) {
        ASSERT_TRUE(b.markRegion(region));
    }

    DirtyBitmap result(geometry);

    ASSERT_EQ(DirtyBitmap::bitwiseAnd(a, b, result), Status::Ok);
    EXPECT_EQ(dirtyRegions(result), (std::vector<std::uint32_t>{20, 30}));

    ASSERT_EQ(DirtyBitmap::bitwiseAndNot(a, b, result), Status::Ok);
    EXPECT_EQ(dirtyRegions(result), (std::vector<std::uint32_t>{10}));

    ASSERT_EQ(DirtyBitmap::bitwiseAndNot(b, a, result), Status::Ok);
    EXPECT_EQ(dirtyRegions(result), (std::vector<std::uint32_t>{40, 50}));

    ASSERT_EQ(DirtyBitmap::bitwiseOr(a, b, result), Status::Ok);
    EXPECT_EQ(dirtyRegions(result), (std::vector<std::uint32_t>{10, 20, 30, 40, 50}));

    ASSERT_EQ(DirtyBitmap::bitwiseXor(a, b, result), Status::Ok);
    EXPECT_EQ(dirtyRegions(result), (std::vector<std::uint32_t>{10, 40, 50}));
}

TEST(DirtyBitmap, BitwiseRejectsMismatchedGeometry)
{
    DirtyBitmap small(makeGeometry(64 * k64K));
    DirtyBitmap otherGranularity(makeGeometry(64 * k64K, 32u * 1024u));
    DirtyBitmap otherCapacity(makeGeometry(128 * k64K));
    DirtyBitmap result(makeGeometry(64 * k64K));

    EXPECT_EQ(DirtyBitmap::bitwiseOr(small, otherGranularity, result), Status::InvalidArgument);
    EXPECT_EQ(DirtyBitmap::bitwiseOr(small, otherCapacity, result), Status::InvalidArgument);
    EXPECT_TRUE(result.none());
}

TEST(DirtyBitmap, BitwiseAllowsOutputAliasingAnInput)
{
    const Geometry geometry = makeGeometry(64 * k64K);
    DirtyBitmap a(geometry);
    DirtyBitmap b(geometry);
    ASSERT_TRUE(a.markRegion(3));
    ASSERT_TRUE(b.markRegion(5));

    ASSERT_EQ(DirtyBitmap::bitwiseOr(a, b, a), Status::Ok);
    EXPECT_EQ(dirtyRegions(a), (std::vector<std::uint32_t>{3, 5}));
}

// ---------------------------------------------------------------------------
// Serialization boundary
// ---------------------------------------------------------------------------

TEST(DirtyBitmap, SetWordsRejectsStrayTailBits)
{
    DirtyBitmap bitmap(makeGeometry(1 * kMiB + 1)); // 17 regions
    std::vector<Word> words(1, Word{1} << 17);

    EXPECT_EQ(bitmap.setWords(words), Status::Corrupt);
    EXPECT_TRUE(bitmap.none());
}

TEST(DirtyBitmap, SetWordsRoundTrips)
{
    const Geometry geometry = makeGeometry(3 * kMiB + 5);
    DirtyBitmap original(geometry);
    ASSERT_EQ(original.markWrite(0, 3 * kMiB), Status::Ok);

    DirtyBitmap restored(geometry);
    ASSERT_EQ(restored.setWords(original.words()), Status::Ok);
    EXPECT_TRUE(restored == original);
}

TEST(DirtyBitmap, SetWordsRejectsWrongWordCount)
{
    DirtyBitmap bitmap(makeGeometry(1 * kMiB)); // 16 regions == exactly one word
    EXPECT_EQ(bitmap.setWords(std::vector<Word>()), Status::Corrupt);
    EXPECT_EQ(bitmap.setWords(std::vector<Word>(2, 0)), Status::Corrupt);
    EXPECT_EQ(bitmap.setWords(std::vector<Word>(1, 0)), Status::Ok);
}

// ---------------------------------------------------------------------------
// Randomized oracle
// ---------------------------------------------------------------------------

TEST(DirtyBitmapRandomized, MatchesSingleThreadedOracle)
{
    // Device whose capacity is not a multiple of the granularity, so the tail
    // region is exercised throughout.
    const std::uint64_t capacity = 4 * kMiB + 12345;
    const Geometry geometry = makeGeometry(capacity);
    DirtyBitmap bitmap(geometry);

    std::vector<char> oracle(geometry.regionCount(), 0);
    std::mt19937_64 rng(0x5EED2026ull);
    const std::uint32_t granularity = geometry.granularityBytes();

    for (int iteration = 0; iteration < 100000; ++iteration) {
        const std::uint64_t offset = rng() % capacity;
        const std::uint64_t remaining = capacity - offset;
        const std::uint64_t maxLength = remaining < 200000 ? remaining : 200000;
        const std::uint64_t length = maxLength == 0 ? 0 : rng() % (maxLength + 1);

        ASSERT_EQ(bitmap.markWrite(offset, length), Status::Ok);
        if (length == 0) {
            continue;
        }

        const std::uint64_t first = offset / granularity;
        const std::uint64_t last = (offset + length - 1) / granularity;
        for (std::uint64_t region = first; region <= last; ++region) {
            oracle[static_cast<std::size_t>(region)] = 1;
        }
    }

    std::uint64_t expectedCount = 0;
    for (std::size_t i = 0; i < oracle.size(); ++i) {
        if (oracle[i] != 0) {
            ++expectedCount;
        }
    }
    EXPECT_EQ(bitmap.countDirty(), expectedCount);

    for (std::uint32_t region = 0; region < geometry.regionCount(); ++region) {
        EXPECT_EQ(bitmap.test(region), oracle[region] != 0) << "region " << region;
    }
}

TEST(DirtyBitmapRandomized, BitwiseOperationsMatchPerBitReference)
{
    const Geometry geometry = makeGeometry(3 * kMiB + 777);
    std::mt19937_64 rng(0xC0FFEEull);

    for (int iteration = 0; iteration < 50; ++iteration) {
        DirtyBitmap a(geometry);
        DirtyBitmap b(geometry);
        std::vector<char> oracleA(geometry.regionCount(), 0);
        std::vector<char> oracleB(geometry.regionCount(), 0);

        for (std::uint32_t region = 0; region < geometry.regionCount(); ++region) {
            if ((rng() & 1u) != 0) {
                ASSERT_TRUE(a.markRegion(region));
                oracleA[region] = 1;
            }
            if ((rng() & 1u) != 0) {
                ASSERT_TRUE(b.markRegion(region));
                oracleB[region] = 1;
            }
        }

        DirtyBitmap result(geometry);

        ASSERT_EQ(DirtyBitmap::bitwiseAnd(a, b, result), Status::Ok);
        for (std::uint32_t region = 0; region < geometry.regionCount(); ++region) {
            const bool expected = oracleA[region] != 0 && oracleB[region] != 0;
            ASSERT_EQ(result.test(region), expected) << "and region " << region;
        }

        ASSERT_EQ(DirtyBitmap::bitwiseAndNot(a, b, result), Status::Ok);
        for (std::uint32_t region = 0; region < geometry.regionCount(); ++region) {
            const bool expected = oracleA[region] != 0 && oracleB[region] == 0;
            ASSERT_EQ(result.test(region), expected) << "andNot region " << region;
        }

        ASSERT_EQ(DirtyBitmap::bitwiseOr(a, b, result), Status::Ok);
        for (std::uint32_t region = 0; region < geometry.regionCount(); ++region) {
            const bool expected = oracleA[region] != 0 || oracleB[region] != 0;
            ASSERT_EQ(result.test(region), expected) << "or region " << region;
        }

        ASSERT_EQ(DirtyBitmap::bitwiseXor(a, b, result), Status::Ok);
        for (std::uint32_t region = 0; region < geometry.regionCount(); ++region) {
            const bool expected = (oracleA[region] != 0) != (oracleB[region] != 0);
            ASSERT_EQ(result.test(region), expected) << "xor region " << region;
        }
    }
}

TEST(DirtyBitmapRandomized, EnumerationAndCountsAgree)
{
    const Geometry geometry = makeGeometry(8 * kMiB + 3);
    std::mt19937_64 rng(0xBEEFull);

    for (int iteration = 0; iteration < 20; ++iteration) {
        DirtyBitmap bitmap(geometry);
        for (std::uint32_t region = 0; region < geometry.regionCount(); ++region) {
            if ((rng() % 3u) == 0) {
                ASSERT_TRUE(bitmap.markRegion(region));
            }
        }

        RegionCollector collector;
        bitmap.forEachRange(collector);

        std::uint64_t covered = 0;
        std::uint32_t previousLast = 0;
        for (std::size_t i = 0; i < collector.spans.size(); ++i) {
            const RegionSpan& span = collector.spans[i];
            ASSERT_LE(span.firstRegion, span.lastRegion);
            if (i > 0) {
                ASSERT_GT(span.firstRegion, previousLast + 1) << "runs must be maximal";
            }
            covered += (span.lastRegion - span.firstRegion) + 1;
            previousLast = span.lastRegion;
        }
        EXPECT_EQ(covered, bitmap.countDirty());

        std::vector<DirtyRange> ranges;
        ASSERT_EQ(bitmap.toRanges(1000, ranges), Status::Ok);
        ASSERT_EQ(ranges.size(), collector.spans.size());

        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            EXPECT_EQ(ranges[i].offsetBytes, collector.spans[i].firstRegion * k64K);
            sum += ranges[i].lengthBytes;
        }
        EXPECT_EQ(sum, bitmap.dirtyBytes());
    }
}

} // namespace
