// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/bitmap.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace blockchronicle {
namespace {

std::size_t wordCountFor(std::uint32_t regionCount)
{
    return (static_cast<std::size_t>(regionCount) + (kBitsPerWord - 1u)) / kBitsPerWord;
}

// Bits at or above regionCount are always zero. The mask is only used to verify
// and to repair the tail after a bitwise operation.
Word tailMaskFor(std::uint32_t regionCount)
{
    const std::uint32_t usedBits = regionCount % kBitsPerWord;
    if (usedBits == 0u) {
        return std::numeric_limits<Word>::max();
    }
    return (Word{1} << usedBits) - Word{1};
}

// Bits [firstBit, lastBit], both inclusive and both < 64.
Word maskInclusive(std::uint32_t firstBit, std::uint32_t lastBit)
{
    const Word low = (firstBit == 0u)
        ? std::numeric_limits<Word>::max()
        : (std::numeric_limits<Word>::max() << firstBit);
    const Word high = (lastBit + 1u >= kBitsPerWord)
        ? std::numeric_limits<Word>::max()
        : ((Word{1} << (lastBit + 1u)) - Word{1});
    return low & high;
}

// Undefined for value == 0; every caller checks first.
std::uint32_t countTrailingZeros(Word value)
{
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<std::uint32_t>(__builtin_ctzll(value));
#else
    std::uint32_t count = 0;
    while ((value & Word{1}) == 0u) {
        value >>= 1u;
        ++count;
    }
    return count;
#endif
}

std::uint32_t popcountOfWord(Word value)
{
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<std::uint32_t>(__builtin_popcountll(value));
#else
    std::uint32_t count = 0;
    while (value != 0u) {
        value &= (value - Word{1});
        ++count;
    }
    return count;
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

Status Geometry::make(std::uint64_t capacityBytes,
                      std::uint32_t granularityBytes,
                      Geometry& out)
{
    if (capacityBytes == 0u || granularityBytes == 0u) {
        return Status::InvalidArgument;
    }

    // ceil(capacity / granularity), computed without overflow: the division and
    // the remainder are both bounded by capacity.
    const std::uint64_t regions = capacityBytes / granularityBytes
        + ((capacityBytes % granularityBytes) != 0u ? 1ull : 0ull);

    if (regions > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return Status::OutOfRange;
    }

    Geometry result;
    result.capacityBytes_ = capacityBytes;
    result.granularityBytes_ = granularityBytes;
    result.regionCount_ = static_cast<std::uint32_t>(regions);
    out = result;
    return Status::Ok;
}

std::uint64_t Geometry::regionOffsetBytes(std::uint32_t region) const
{
    return static_cast<std::uint64_t>(region) * static_cast<std::uint64_t>(granularityBytes_);
}

std::uint64_t Geometry::regionLengthBytes(std::uint32_t region) const
{
    const std::uint64_t offset = regionOffsetBytes(region);
    if (offset >= capacityBytes_) {
        return 0u;
    }
    const std::uint64_t remaining = capacityBytes_ - offset;
    return remaining < static_cast<std::uint64_t>(granularityBytes_)
        ? remaining
        : static_cast<std::uint64_t>(granularityBytes_);
}

bool Geometry::equals(const Geometry& other) const
{
    return capacityBytes_ == other.capacityBytes_
        && granularityBytes_ == other.granularityBytes_
        && regionCount_ == other.regionCount_;
}

Status Geometry::spanOf(std::uint64_t offsetBytes,
                        std::uint64_t lengthBytes,
                        RegionSpan& out) const
{
    if (lengthBytes == 0u) {
        // Zero-length writes are legal no-ops handled by the caller. Treating
        // them as an error here keeps this function's contract total.
        return Status::InvalidArgument;
    }
    if (offsetBytes >= capacityBytes_) {
        return Status::OutOfRange;
    }
    // Compare against the remaining capacity instead of computing
    // offset + length, which would overflow.
    if (lengthBytes > capacityBytes_ - offsetBytes) {
        return Status::OutOfRange;
    }

    const std::uint64_t first = offsetBytes / static_cast<std::uint64_t>(granularityBytes_);
    const std::uint64_t last = (offsetBytes + lengthBytes - 1u)
        / static_cast<std::uint64_t>(granularityBytes_);

    if (last >= static_cast<std::uint64_t>(regionCount_)) {
        return Status::OutOfRange; // defensive; unreachable given the checks above
    }

    out.firstRegion = static_cast<std::uint32_t>(first);
    out.lastRegion = static_cast<std::uint32_t>(last);
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// DirtyBitmap
// ---------------------------------------------------------------------------

DirtyBitmap::DirtyBitmap(const Geometry& geometry)
    : geometry_(geometry)
    , words_(wordCountFor(geometry.regionCount()), 0u)
{
}

bool DirtyBitmap::markRegion(std::uint32_t region)
{
    if (region >= geometry_.regionCount()) {
        return false;
    }
    const std::size_t wordIndex = static_cast<std::size_t>(region) / kBitsPerWord;
    const std::uint32_t bit = region % kBitsPerWord;
    words_[wordIndex] |= (Word{1} << bit);
    return true;
}

bool DirtyBitmap::markRegionRange(std::uint32_t firstRegion, std::uint32_t lastRegion)
{
    if (firstRegion > lastRegion || lastRegion >= geometry_.regionCount()) {
        return false;
    }

    const std::uint32_t firstWord = firstRegion / kBitsPerWord;
    const std::uint32_t lastWord = lastRegion / kBitsPerWord;

    for (std::uint32_t wordIndex = firstWord; wordIndex <= lastWord; ++wordIndex) {
        const std::uint32_t firstBit = (wordIndex == firstWord)
            ? (firstRegion % kBitsPerWord)
            : 0u;
        const std::uint32_t lastBit = (wordIndex == lastWord)
            ? (lastRegion % kBitsPerWord)
            : (kBitsPerWord - 1u);
        words_[wordIndex] |= maskInclusive(firstBit, lastBit);
    }
    return true;
}

Status DirtyBitmap::markWrite(std::uint64_t offsetBytes, std::uint64_t lengthBytes)
{
    if (lengthBytes == 0u) {
        return Status::Ok; // legal no-op
    }

    RegionSpan span;
    const Status status = geometry_.spanOf(offsetBytes, lengthBytes, span);
    if (status != Status::Ok) {
        return status; // nothing has been marked
    }

    markRegionRange(span.firstRegion, span.lastRegion);
    return Status::Ok;
}

bool DirtyBitmap::test(std::uint32_t region) const
{
    if (region >= geometry_.regionCount()) {
        return false;
    }
    const std::size_t wordIndex = static_cast<std::size_t>(region) / kBitsPerWord;
    const std::uint32_t bit = region % kBitsPerWord;
    return (words_[wordIndex] & (Word{1} << bit)) != 0u;
}

std::uint64_t DirtyBitmap::countDirty() const
{
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < words_.size(); ++i) {
        total += popcountOfWord(words_[i]);
    }
    return total;
}

std::uint64_t DirtyBitmap::dirtyBytes() const
{
    const std::uint64_t dirtyRegions = countDirty();
    if (dirtyRegions == 0u) {
        return 0u;
    }

    const std::uint64_t full = static_cast<std::uint64_t>(geometry_.granularityBytes());
    std::uint64_t bytes = dirtyRegions * full;

    const std::uint32_t lastRegion = geometry_.regionCount() - 1u;
    if (test(lastRegion)) {
        bytes -= full - geometry_.regionLengthBytes(lastRegion);
    }
    return bytes;
}

bool DirtyBitmap::any() const
{
    for (std::size_t i = 0; i < words_.size(); ++i) {
        if (words_[i] != 0u) {
            return true;
        }
    }
    return false;
}

bool DirtyBitmap::none() const
{
    return !any();
}

void DirtyBitmap::clear()
{
    std::fill(words_.begin(), words_.end(), 0u);
}

void DirtyBitmap::forEachRange(RangeVisitor& visitor) const
{
    const std::uint32_t regionCount = geometry_.regionCount();
    std::uint32_t index = 0;

    while (index < regionCount) {
        const std::size_t wordIndex = static_cast<std::size_t>(index) / kBitsPerWord;
        const std::uint32_t bitOffset = index % kBitsPerWord;

        // Skip the bits below index in this word so a run is never re-emitted.
        const Word masked = words_[wordIndex]
            & (std::numeric_limits<Word>::max() << bitOffset);

        if (masked == 0u) {
            const std::uint64_t next = static_cast<std::uint64_t>(wordIndex + 1u) * kBitsPerWord;
            index = (next >= static_cast<std::uint64_t>(regionCount))
                ? regionCount
                : static_cast<std::uint32_t>(next);
            continue;
        }

        const std::uint32_t start = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(wordIndex) * kBitsPerWord
            + countTrailingZeros(masked));

        std::uint32_t end = start;
        while ((end + 1u) < regionCount && test(end + 1u)) {
            ++end;
        }

        visitor.onRegionRange(start, end);
        index = end + 1u;
    }
}

namespace {

// Converts region runs to byte ranges. Appends nothing once the limit is hit and
// records the failure so toRanges can leave the caller's vector untouched.
class RangeBuilder : public RangeVisitor {
public:
    RangeBuilder(const Geometry& geometry,
                 std::uint64_t limit,
                 std::vector<DirtyRange>& out,
                 Status& status)
        : geometry_(geometry)
        , limit_(limit)
        , out_(out)
        , status_(status)
    {
    }

    void onRegionRange(std::uint32_t firstRegion, std::uint32_t lastRegion) override
    {
        if (status_ != Status::Ok) {
            return;
        }
        if (static_cast<std::uint64_t>(out_.size()) >= limit_) {
            status_ = Status::OutOfRange;
            return;
        }

        // Regions are contiguous, so the range end is the last region's offset
        // plus its clamped length. O(1) regardless of run length.
        const std::uint64_t offset = geometry_.regionOffsetBytes(firstRegion);
        const std::uint64_t end = geometry_.regionOffsetBytes(lastRegion)
            + geometry_.regionLengthBytes(lastRegion);

        DirtyRange range;
        range.offsetBytes = offset;
        range.lengthBytes = end - offset;
        out_.push_back(range);
    }

private:
    const Geometry& geometry_;
    std::uint64_t limit_;
    std::vector<DirtyRange>& out_;
    Status& status_;
};

} // namespace

Status DirtyBitmap::toRanges(std::uint64_t maxRanges, std::vector<DirtyRange>& out) const
{
    std::vector<DirtyRange> collected;
    collected.reserve(16);

    Status status = Status::Ok;
    RangeBuilder builder(geometry_, maxRanges, collected, status);
    forEachRange(builder);

    if (status != Status::Ok) {
        return status; // out untouched
    }

    out = std::move(collected);
    return Status::Ok;
}

Status DirtyBitmap::combine(const DirtyBitmap& a,
                            const DirtyBitmap& b,
                            BitwiseOp op,
                            DirtyBitmap& out)
{
    if (!a.geometry_.equals(b.geometry_)) {
        return Status::InvalidArgument;
    }

    DirtyBitmap result(a.geometry_);

    for (std::size_t i = 0; i < result.words_.size(); ++i) {
        switch (op) {
        case BitwiseOp::And:
            result.words_[i] = a.words_[i] & b.words_[i];
            break;
        case BitwiseOp::Or:
            result.words_[i] = a.words_[i] | b.words_[i];
            break;
        case BitwiseOp::AndNot:
            result.words_[i] = a.words_[i] & ~b.words_[i];
            break;
        case BitwiseOp::Xor:
            result.words_[i] = a.words_[i] ^ b.words_[i];
            break;
        }
    }

    if (!result.words_.empty()) {
        result.words_.back() &= tailMaskFor(result.geometry_.regionCount());
    }

    out = std::move(result);
    return Status::Ok;
}

Status DirtyBitmap::bitwiseAnd(const DirtyBitmap& a,
                               const DirtyBitmap& b,
                               DirtyBitmap& out)
{
    return combine(a, b, BitwiseOp::And, out);
}

Status DirtyBitmap::bitwiseOr(const DirtyBitmap& a,
                              const DirtyBitmap& b,
                              DirtyBitmap& out)
{
    return combine(a, b, BitwiseOp::Or, out);
}

Status DirtyBitmap::bitwiseAndNot(const DirtyBitmap& a,
                                  const DirtyBitmap& b,
                                  DirtyBitmap& out)
{
    return combine(a, b, BitwiseOp::AndNot, out);
}

Status DirtyBitmap::bitwiseXor(const DirtyBitmap& a,
                               const DirtyBitmap& b,
                               DirtyBitmap& out)
{
    return combine(a, b, BitwiseOp::Xor, out);
}

Status DirtyBitmap::setWords(std::vector<Word> words)
{
    const std::size_t expected = wordCountFor(geometry_.regionCount());
    if (words.size() != expected) {
        return Status::Corrupt;
    }

    if (!words.empty()) {
        const Word tailMask = tailMaskFor(geometry_.regionCount());
        if ((words.back() & ~tailMask) != 0u) {
            // A persisted bitmap with bits beyond the device is corrupt, not a
            // cosmetic issue: masking it would hide a format or identity error.
            return Status::Corrupt;
        }
    }

    words_ = std::move(words);
    return Status::Ok;
}

bool DirtyBitmap::operator==(const DirtyBitmap& other) const
{
    return geometry_.equals(other.geometry_) && words_ == other.words_;
}

bool DirtyBitmap::operator!=(const DirtyBitmap& other) const
{
    return !(*this == other);
}

} // namespace blockchronicle
