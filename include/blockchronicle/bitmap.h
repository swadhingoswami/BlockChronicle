// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_BITMAP_H
#define BLOCKCHRONICLE_BITMAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "blockchronicle/types.h"

namespace blockchronicle {

// Maps byte offsets to tracking regions and is the single authority for region
// arithmetic. Every overflow check in the project lives here; callers never do
// offset arithmetic themselves.
class Geometry {
public:
    Geometry() = default;

    // capacityBytes must be > 0, granularityBytes must be > 0, and the derived
    // region count must fit in 32 bits. Returns InvalidArgument or OutOfRange.
    static Status make(std::uint64_t capacityBytes,
                       std::uint32_t granularityBytes,
                       Geometry& out);

    std::uint64_t capacityBytes() const { return capacityBytes_; }
    std::uint32_t granularityBytes() const { return granularityBytes_; }
    std::uint32_t regionCount() const { return regionCount_; }

    // Byte offset of the region, and its length clamped to the device capacity
    // so that the final partial region reports only its real bytes.
    std::uint64_t regionOffsetBytes(std::uint32_t region) const;
    std::uint64_t regionLengthBytes(std::uint32_t region) const;

    bool equals(const Geometry& other) const;

    // Maps a write to the inclusive span of regions it touches.
    // Preconditions: lengthBytes > 0. Callers must handle zero-length writes
    // before calling (they are legal no-ops, not errors).
    // Returns OutOfRange if the write does not fit in the device or if
    // offsetBytes + lengthBytes would overflow.
    Status spanOf(std::uint64_t offsetBytes,
                  std::uint64_t lengthBytes,
                  RegionSpan& out) const;

private:
    std::uint64_t capacityBytes_ = 0;
    std::uint32_t granularityBytes_ = 0;
    std::uint32_t regionCount_ = 0;
};

// Callback for coalesced region runs. An interface rather than std::function so
// that enumeration stays allocation-free.
class RangeVisitor {
public:
    virtual ~RangeVisitor() = default;
    virtual void onRegionRange(std::uint32_t firstRegion, std::uint32_t lastRegion) = 0;
};

// A fixed-size set of dirty regions, one bit per region, backed by 64-bit words.
//
// Thread safety: DirtyBitmap is NOT internally synchronised. Synchronisation is
// owned by the caller one level up (see docs/concurrency.md); a per-bitmap mutex
// would make atomic bitmap comparison impossible.
//
// Invariant: bits at indices >= regionCount() are always zero. Every operation
// preserves this, so popcount is exact and enumeration never yields a region
// outside the device.
class DirtyBitmap {
public:
    explicit DirtyBitmap(const Geometry& geometry);

    const Geometry& geometry() const { return geometry_; }
    std::uint32_t regionCount() const { return geometry_.regionCount(); }
    std::uint32_t granularityBytes() const { return geometry_.granularityBytes(); }
    std::size_t wordCount() const { return words_.size(); }

    // Return false if the region index is out of range; no bit is changed.
    bool markRegion(std::uint32_t region);
    bool markRegionRange(std::uint32_t firstRegion, std::uint32_t lastRegion);

    // Validates and marks. A zero-length write is Ok and marks nothing.
    // A write that is partially out of range marks nothing at all.
    Status markWrite(std::uint64_t offsetBytes, std::uint64_t lengthBytes);

    bool test(std::uint32_t region) const;

    std::uint64_t countDirty() const;
    std::uint64_t dirtyBytes() const;
    bool any() const;
    bool none() const;
    void clear();

    // Coalesced runs of dirty regions, in ascending order.
    void forEachRange(RangeVisitor& visitor) const;

    // All-or-nothing: if more than maxRanges runs exist, returns OutOfRange and
    // leaves out untouched.
    Status toRanges(std::uint64_t maxRanges, std::vector<DirtyRange>& out) const;

    // Bitwise operations. All require identical geometry (capacity, granularity
    // and region count) and return InvalidArgument otherwise. The tail bits above
    // regionCount() are masked after every operation.
    static Status bitwiseAnd(const DirtyBitmap& a, const DirtyBitmap& b, DirtyBitmap& out);
    static Status bitwiseOr(const DirtyBitmap& a, const DirtyBitmap& b, DirtyBitmap& out);
    static Status bitwiseAndNot(const DirtyBitmap& a, const DirtyBitmap& b, DirtyBitmap& out);
    static Status bitwiseXor(const DirtyBitmap& a, const DirtyBitmap& b, DirtyBitmap& out);

    // Storage access for the persistence layer. setWords validates the word count
    // and rejects stray bits above the tail with Status::Corrupt rather than
    // silently masking them.
    const std::vector<Word>& words() const { return words_; }
    Status setWords(std::vector<Word> words);

    bool operator==(const DirtyBitmap& other) const;
    bool operator!=(const DirtyBitmap& other) const;

private:
    enum class BitwiseOp { And, Or, AndNot, Xor };

    static Status combine(const DirtyBitmap& a,
                          const DirtyBitmap& b,
                          BitwiseOp op,
                          DirtyBitmap& out);

    Geometry geometry_;
    std::vector<Word> words_;
};

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_BITMAP_H
