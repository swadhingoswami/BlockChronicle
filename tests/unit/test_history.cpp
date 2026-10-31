// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Phase 3 acceptance tests: union semantics, the cumulative checkpoint grid,
// and the property that the accelerated path is indistinguishable from the
// definition.
//
// Definition under test:
//     unionRange(from, to) == OR of D_g for g in (from, to]
//
// Accelerated path under test:
//     with checkpoints every K, the work is bounded by K deltas plus, when the
//     upper boundary is a checkpoint, two bitmap operations.
//
// The property test compares the two directly. If the acceleration ever
// diverges from the definition, that test fails.

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "blockchronicle/generation.h"
#include "blockchronicle/history.h"
#include "support/status_printer.h"

namespace {

using namespace blockchronicle;

constexpr std::uint32_t k64K = 64u * 1024u;

Geometry makeGeometry(std::uint64_t capacityBytes = 256 * k64K,
                      std::uint32_t granularity = k64K)
{
    Geometry geometry;
    const Status status = Geometry::make(capacityBytes, granularity, geometry);
    EXPECT_EQ(status, Status::Ok);
    return geometry;
}

DirtyBitmap makeBitmap(const Geometry& geometry, const std::vector<std::uint32_t>& regions)
{
    DirtyBitmap bitmap(geometry);
    for (std::size_t i = 0; i < regions.size(); ++i) {
        EXPECT_TRUE(bitmap.markRegion(regions[i]));
    }
    return bitmap;
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

// The definition, computed the slow obvious way: one OR per generation.
DirtyBitmap naiveUnion(const HistoryStore& store,
                       const Geometry& geometry,
                       Generation fromExclusive,
                       Generation toInclusive)
{
    DirtyBitmap result(geometry);
    for (Generation generation = fromExclusive + 1u; generation <= toInclusive; ++generation) {
        SealedDelta delta;
        EXPECT_EQ(store.delta(generation, delta), Status::Ok);
        EXPECT_EQ(DirtyBitmap::bitwiseOr(result, *delta, result), Status::Ok);
    }
    return result;
}

class NullSink : public SealSink {
public:
    Status commitDelta(Generation, const DirtyBitmap&) override { return Status::Ok; }
};

class FailingSink : public SealSink {
public:
    Status commitDelta(Generation, const DirtyBitmap&) override { return Status::IoError; }
};

// Builds a store with one delta per generation, each containing the regions the
// generator returns for that generation.
class HistoryFixture {
public:
    HistoryFixture()
        : geometry_(makeGeometry())
        , store_(geometry_, &sink_)
    {
    }

    void commit(Generation generation, const std::vector<std::uint32_t>& regions)
    {
        const DirtyBitmap bitmap = makeBitmap(geometry_, regions);
        const Status status = store_.commitDelta(generation, bitmap);
        EXPECT_EQ(status, Status::Ok);
    }

    Geometry geometry_;
    NullSink sink_;
    HistoryStore store_;
};

// ---------------------------------------------------------------------------
// Range semantics
// ---------------------------------------------------------------------------

TEST(HistoryStore, EmptyStoreReturnsNotFoundForNonEmptyRange)
{
    HistoryStore store(makeGeometry());

    DirtyBitmap out(makeGeometry());
    EXPECT_EQ(store.unionRange(0, 1, out), Status::NotFound);
    EXPECT_EQ(store.newestGeneration(), 0u);
    EXPECT_EQ(store.oldestGeneration(), 0u);
}

TEST(HistoryStore, UnionIsExclusiveFromInclusiveTo)
{
    HistoryFixture fixture;
    fixture.commit(1, {1});
    fixture.commit(2, {2, 3});
    fixture.commit(3, {3, 4});

    DirtyBitmap out(fixture.geometry_);

    ASSERT_EQ(fixture.store_.unionRange(0, 3, out), Status::Ok);
    EXPECT_EQ(dirtyRegions(out), (std::vector<std::uint32_t>{1, 2, 3, 4}));

    ASSERT_EQ(fixture.store_.unionRange(1, 3, out), Status::Ok);
    EXPECT_EQ(dirtyRegions(out), (std::vector<std::uint32_t>{2, 3, 4}));

    // Generation 3's own delta, not the cumulative.
    ASSERT_EQ(fixture.store_.unionRange(2, 3, out), Status::Ok);
    EXPECT_EQ(dirtyRegions(out), (std::vector<std::uint32_t>{3, 4}));
}

TEST(HistoryStore, EmptyIntervalIsOkAndEmpty)
{
    HistoryFixture fixture;
    fixture.commit(1, {5});

    DirtyBitmap out(fixture.geometry_);
    ASSERT_EQ(fixture.store_.unionRange(1, 1, out), Status::Ok);
    EXPECT_TRUE(out.none());
}

TEST(HistoryStore, InvertedRangeIsInvalidArgument)
{
    HistoryFixture fixture;
    fixture.commit(1, {5});
    fixture.commit(2, {6});

    DirtyBitmap out(fixture.geometry_);
    EXPECT_EQ(fixture.store_.unionRange(2, 1, out), Status::InvalidArgument);
}

TEST(HistoryStore, UnsealedBoundaryIsNotFoundNotPartial)
{
    HistoryFixture fixture;
    fixture.commit(1, {5});
    fixture.commit(2, {6});

    DirtyBitmap out(fixture.geometry_);
    EXPECT_EQ(fixture.store_.unionRange(0, 3, out), Status::NotFound);

    // And the out parameter must not contain a partial answer.
    EXPECT_TRUE(out.none());
}

TEST(HistoryStore, ReclaimedHistoryIsNotFound)
{
    HistoryFixture fixture;
    fixture.commit(1, {5});
    fixture.commit(2, {6});
    fixture.commit(3, {7});

    ASSERT_TRUE(fixture.store_.erase(1));

    DirtyBitmap out(fixture.geometry_);
    EXPECT_EQ(fixture.store_.unionRange(0, 3, out), Status::NotFound)
        << "a query starting before retained history must be refused, not truncated";

    ASSERT_EQ(fixture.store_.unionRange(1, 3, out), Status::Ok);
    EXPECT_EQ(dirtyRegions(out), (std::vector<std::uint32_t>{6, 7}));
}

TEST(HistoryStore, NonContiguousCommitIsRejected)
{
    HistoryFixture fixture;
    fixture.commit(1, {5});

    DirtyBitmap bitmap = makeBitmap(fixture.geometry_, {6});
    EXPECT_EQ(fixture.store_.commitDelta(3, bitmap), Status::InvalidState);
    EXPECT_EQ(fixture.store_.newestGeneration(), 1u);
}

TEST(HistoryStore, FailedDurableCommitIsNotPublished)
{
    const Geometry geometry = makeGeometry();
    FailingSink sink;
    HistoryStore store(geometry, &sink);

    const DirtyBitmap bitmap = makeBitmap(geometry, {1, 2});
    EXPECT_EQ(store.commitDelta(1, bitmap), Status::IoError);
    EXPECT_EQ(store.newestGeneration(), 0u);
    EXPECT_EQ(store.deltaCount(), 0u);

    SealedDelta delta;
    EXPECT_EQ(store.delta(1, delta), Status::NotFound);
}

TEST(HistoryStore, RejectsMismatchedGeometry)
{
    HistoryStore store(makeGeometry());
    const DirtyBitmap other(makeGeometry(512 * k64K));
    EXPECT_EQ(store.commitDelta(1, other), Status::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Checkpoint grid
// ---------------------------------------------------------------------------

TEST(HistoryStore, CheckpointsAppearEveryInterval)
{
    HistoryFixture fixture;
    fixture.store_.setCheckpointInterval(4);
    for (Generation generation = 1; generation <= 10; ++generation) {
        fixture.commit(generation, {static_cast<std::uint32_t>(generation)});
    }

    EXPECT_EQ(fixture.store_.checkpoints(), (std::vector<Generation>{4, 8}));
}

TEST(HistoryStore, CumulativeEqualsUnionFromTheFirstGeneration)
{
    HistoryFixture fixture;
    fixture.store_.setCheckpointInterval(4);
    for (Generation generation = 1; generation <= 8; ++generation) {
        fixture.commit(generation, {static_cast<std::uint32_t>(generation)});
    }

    SealedDelta cumulative;
    ASSERT_EQ(fixture.store_.cumulative(8, cumulative), Status::Ok);
    EXPECT_EQ(dirtyRegions(*cumulative),
              (std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6, 7, 8}));

    const DirtyBitmap byDefinition = naiveUnion(fixture.store_, fixture.geometry_, 0, 8);
    EXPECT_TRUE(*cumulative == byDefinition);
}

// The specification claimed: union(a, b) == cumulative(b) AND NOT cumulative(a).
// It is false, and the failure is silent: a region that changed both before and
// after `a` is dirty in both cumulatives and therefore cancelled out.
//
// This test encodes the counterexample so the shortcut cannot be reintroduced.
TEST(HistoryStore, CumulativeDifferenceIsNotTheIncrementalUnion)
{
    HistoryFixture fixture;
    fixture.store_.setCheckpointInterval(4);

    // Region 7 changes before the cursor and again after it. Regions 1 and 2
    // change only in the second period.
    fixture.commit(1, {7});
    fixture.commit(2, {7});
    fixture.commit(3, {7});
    fixture.commit(4, {7});

    fixture.commit(5, {1, 7});
    fixture.commit(6, {2});
    fixture.commit(7, {});
    fixture.commit(8, {});

    SealedDelta lower;
    SealedDelta upper;
    ASSERT_EQ(fixture.store_.cumulative(4, lower), Status::Ok);
    ASSERT_EQ(fixture.store_.cumulative(8, upper), Status::Ok);
    ASSERT_TRUE(lower->test(7));
    ASSERT_TRUE(upper->test(7));

    DirtyBitmap difference(fixture.geometry_);
    ASSERT_EQ(DirtyBitmap::bitwiseAndNot(*upper, *lower, difference), Status::Ok);

    DirtyBitmap byDefinition(fixture.geometry_);
    ASSERT_EQ(fixture.store_.unionRange(4, 8, byDefinition), Status::Ok);

    // The incremental set must contain 7, because 7 changed after the cursor.
    EXPECT_TRUE(byDefinition.test(7));
    EXPECT_TRUE(byDefinition.test(1));
    EXPECT_TRUE(byDefinition.test(2));

    // The cumulative difference lost region 7 entirely.
    EXPECT_FALSE(difference.test(7))
        << "if this ever becomes true, the spec's formula changed meaning";
    EXPECT_FALSE(difference == byDefinition)
        << "cumulative(b) AND NOT cumulative(a) must not be mistaken for "
           "the incremental union";

    // They agree only when no region is dirty on both sides, which is exactly
    // why the bug survives casual testing.
    EXPECT_EQ(dirtyRegions(difference), (std::vector<std::uint32_t>{1, 2}));
    EXPECT_EQ(dirtyRegions(byDefinition), (std::vector<std::uint32_t>{1, 2, 7}));
}

TEST(HistoryStore, UnionMatchesNaiveOracleOnRandomHistory)
{
    const Geometry geometry = makeGeometry(4096 * k64K);
    NullSink sink;
    HistoryStore store(geometry, &sink);
    store.setCheckpointInterval(8);

    constexpr Generation kGenerations = 120;
    std::mt19937_64 rng(0xA11CE2026ull);

    for (Generation generation = 1; generation <= kGenerations; ++generation) {
        DirtyBitmap bitmap(geometry);
        for (std::uint32_t region = 0; region < 64; ++region) {
            if ((rng() % 5u) == 0u) {
                ASSERT_TRUE(bitmap.markRegion(static_cast<std::uint32_t>(rng() % 4096u)));
            }
        }
        ASSERT_EQ(store.commitDelta(generation, bitmap), Status::Ok);
    }

    for (int trial = 0; trial < 400; ++trial) {
        // Random bounds, including single-generation and full-history ranges,
        // to catch inclusive/exclusive off-by-one errors.
        const Generation from = rng() % kGenerations;
        Generation to = from + 1u + (rng() % 40u);
        if (to > kGenerations) {
            to = kGenerations;
        }

        DirtyBitmap actual(geometry);
        ASSERT_EQ(store.unionRange(from, to, actual), Status::Ok)
            << "range (" << from << ", " << to << "]";

        const DirtyBitmap expected = naiveUnion(store, geometry, from, to);
        ASSERT_TRUE(actual == expected)
            << "union diverged from the oracle for range (" << from << ", " << to << "]";
    }
}

TEST(HistoryStore, UnionIsComposableAcrossSplitPoints)
{
    const Geometry geometry = makeGeometry(4096 * k64K);
    NullSink sink;
    HistoryStore store(geometry, &sink);
    store.setCheckpointInterval(8);

    std::mt19937 rng(0xB0B0u);
    for (Generation generation = 1; generation <= 64; ++generation) {
        DirtyBitmap bitmap(geometry);
        for (std::uint32_t i = 0; i < 20; ++i) {
            ASSERT_TRUE(bitmap.markRegion(static_cast<std::uint32_t>(rng() % 4096u)));
        }
        ASSERT_EQ(store.commitDelta(generation, bitmap), Status::Ok);
    }

    for (int trial = 0; trial < 100; ++trial) {
        Generation a = rng() % 60u;
        Generation b = a + 1u + (rng() % 3u);
        Generation c = b + 1u + (rng() % 3u);
        if (c > 64u) {
            c = 64u;
        }

        DirtyBitmap whole(geometry);
        DirtyBitmap left(geometry);
        DirtyBitmap right(geometry);
        DirtyBitmap combined(geometry);

        ASSERT_EQ(store.unionRange(a, c, whole), Status::Ok);
        ASSERT_EQ(store.unionRange(a, b, left), Status::Ok);
        ASSERT_EQ(store.unionRange(b, c, right), Status::Ok);
        ASSERT_EQ(DirtyBitmap::bitwiseOr(left, right, combined), Status::Ok);

        ASSERT_TRUE(whole == combined)
            << "union(" << a << "," << c << ") != union(" << a << "," << b
            << ") | union(" << b << "," << c << ")";
    }
}

TEST(HistoryStore, RebuildCheckpointsProducesIdenticalResults)
{
    const Geometry geometry = makeGeometry(4096 * k64K);
    NullSink sink;
    HistoryStore store(geometry, &sink);
    store.setCheckpointInterval(8);

    std::mt19937 rng(0x5EED5EEDu);
    for (Generation generation = 1; generation <= 40; ++generation) {
        DirtyBitmap bitmap(geometry);
        for (std::uint32_t i = 0; i < 30; ++i) {
            ASSERT_TRUE(bitmap.markRegion(static_cast<std::uint32_t>(rng() % 4096u)));
        }
        ASSERT_EQ(store.commitDelta(generation, bitmap), Status::Ok);
    }

    std::vector<DirtyBitmap> before;
    for (Generation from = 0; from <= 40; from += 7) {
        for (Generation to = from; to <= 40; to += 11) {
            DirtyBitmap out(geometry);
            ASSERT_EQ(store.unionRange(from, to, out), Status::Ok);
            before.push_back(out);
        }
    }

    ASSERT_EQ(store.rebuildCheckpoints(), Status::Ok);

    std::size_t index = 0;
    for (Generation from = 0; from <= 40; from += 7) {
        for (Generation to = from; to <= 40; to += 11) {
            DirtyBitmap out(geometry);
            ASSERT_EQ(store.unionRange(from, to, out), Status::Ok);
            ASSERT_TRUE(out == before[index]) << "rebuild changed results for ("
                                              << from << ", " << to << "]";
            ++index;
        }
    }
}

TEST(HistoryStore, ChangingIntervalInvalidatesCheckpointsOnly)
{
    HistoryFixture fixture;
    fixture.store_.setCheckpointInterval(4);
    for (Generation generation = 1; generation <= 8; ++generation) {
        fixture.commit(generation, {static_cast<std::uint32_t>(generation)});
    }
    ASSERT_EQ(fixture.store_.checkpoints(), (std::vector<Generation>{4, 8}));

    fixture.store_.setCheckpointInterval(2);
    EXPECT_TRUE(fixture.store_.checkpoints().empty());

    // Correctness is unaffected while the grid is empty.
    DirtyBitmap out(fixture.geometry_);
    ASSERT_EQ(fixture.store_.unionRange(2, 8, out), Status::Ok);
    EXPECT_EQ(dirtyRegions(out), (std::vector<std::uint32_t>{3, 4, 5, 6, 7, 8}));

    // The next commit rebuilds the grid at the new interval.
    fixture.commit(9, {9});
    EXPECT_EQ(fixture.store_.checkpoints(),
              (std::vector<Generation>{2, 4, 6, 8}));
}

// ---------------------------------------------------------------------------
// Retention interaction
// ---------------------------------------------------------------------------

TEST(HistoryStore, EraseOnlyRemovesTheOldestGeneration)
{
    HistoryFixture fixture;
    fixture.commit(1, {1});
    fixture.commit(2, {2});
    fixture.commit(3, {3});

    EXPECT_FALSE(fixture.store_.erase(2));
    EXPECT_FALSE(fixture.store_.erase(99));
    EXPECT_TRUE(fixture.store_.erase(1));
    EXPECT_FALSE(fixture.store_.erase(1));
    EXPECT_EQ(fixture.store_.oldestGeneration(), 2u);
    EXPECT_EQ(fixture.store_.newestGeneration(), 3u);
}

TEST(HistoryStore, QueriesStayCorrectAfterReclaimingTheOldestGeneration)
{
    const Geometry geometry = makeGeometry(4096 * k64K);
    NullSink sink;
    HistoryStore store(geometry, &sink);
    store.setCheckpointInterval(4);

    std::mt19937 rng(0xD00Du);
    for (Generation generation = 1; generation <= 40; ++generation) {
        DirtyBitmap bitmap(geometry);
        for (std::uint32_t i = 0; i < 25; ++i) {
            ASSERT_TRUE(bitmap.markRegion(static_cast<std::uint32_t>(rng() % 4096u)));
        }
        ASSERT_EQ(store.commitDelta(generation, bitmap), Status::Ok);
    }

    for (Generation generation = 1; generation <= 20; ++generation) {
        ASSERT_TRUE(store.erase(generation));
    }

    // The next commit forces a rebuild with the new baseline.
    {
        DirtyBitmap bitmap(geometry);
        ASSERT_TRUE(bitmap.markRegion(123));
        ASSERT_EQ(store.commitDelta(41, bitmap), Status::Ok);
    }

    const std::vector<Generation> checkpointGenerations = store.checkpoints();
    EXPECT_FALSE(checkpointGenerations.empty());

    for (Generation from = 20; from < 41; from += 5) {
        DirtyBitmap out(geometry);
        ASSERT_EQ(store.unionRange(from, 41, out), Status::Ok);
        const DirtyBitmap expected = naiveUnion(store, geometry, from, 41);
        ASSERT_TRUE(out == expected) << "post-reclaim union diverged for (" << from
                                     << ", 41]";
    }

    // The fast path through rebuilt checkpoints must agree with the definition.
    DirtyBitmap viaFastPath(geometry);
    ASSERT_EQ(store.unionRange(24, 40, viaFastPath), Status::Ok);
    const DirtyBitmap expected = naiveUnion(store, geometry, 24, 40);
    EXPECT_TRUE(viaFastPath == expected);
}

// ---------------------------------------------------------------------------
// Integration with the generation lifecycle
// ---------------------------------------------------------------------------

TEST(HistoryStore, IntegratesWithGenerationManagerSeals)
{
    const Geometry geometry = makeGeometry();
    NullSink sink;
    HistoryStore store(geometry, &sink);
    GenerationManager manager(store, 1);

    ASSERT_EQ(manager.markWrite(10 * k64K, 1), Status::Ok);
    ASSERT_EQ(manager.seal(), Status::Ok);

    ASSERT_EQ(manager.markWrite(20 * k64K, 1), Status::Ok);
    ASSERT_EQ(manager.markWrite(30 * k64K, 1), Status::Ok);
    ASSERT_EQ(manager.seal(), Status::Ok);

    DirtyBitmap out(geometry);
    ASSERT_EQ(store.unionRange(0, 2, out), Status::Ok);
    EXPECT_EQ(dirtyRegions(out), (std::vector<std::uint32_t>{10, 20, 30}));

    ASSERT_EQ(store.unionRange(1, 2, out), Status::Ok);
    EXPECT_EQ(dirtyRegions(out), (std::vector<std::uint32_t>{20, 30}));

    // Union over a range whose upper bound is the active, unsealed generation
    // must be refused.
    EXPECT_EQ(store.unionRange(0, 3, out), Status::NotFound);
    EXPECT_EQ(manager.activeGeneration(), 3u);
}

} // namespace
