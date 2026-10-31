// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_HISTORY_H
#define BLOCKCHRONICLE_HISTORY_H

#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <vector>

#include "blockchronicle/bitmap.h"
#include "blockchronicle/seal.h"
#include "blockchronicle/types.h"

namespace blockchronicle {

// Owns the sealed delta history for one device: the deltas themselves, the
// cumulative checkpoint grid, and retention.
//
// Two bitmap kinds, deliberately asymmetric (docs/bitmap-history.md section 3):
//
//   Deltas are the source of truth. They are the unit of retention, replay and
//   garbage collection, and they are the only structure that can answer "what
//   changed in generation G".
//
//   Cumulative checkpoints are a derived analysis structure, stored every
//   `checkpointInterval()` deltas. They answer "has this region ever changed
//   since the baseline". They can always be rebuilt from the deltas and may be
//   deleted at any time without affecting correctness.
//
//   Critical: a device-wide cumulative bitmap must NEVER be used to compute an
//   incremental range. `cumulative(b) AND NOT cumulative(a)` yields "changed in
//   (a, b] and never before a", which drops every region that changed on both
//   sides of the cursor -- a false negative, i.e. data loss. The only correct
//   acceleration is a per-consumer pending bitmap, because "changed since C"
//   depends on C. That lives in the consumer layer (Phase 4).
//
// Retained range invariant: generations in [oldestGeneration(),
// newestGeneration()] are contiguous and every one of them is present. The
// store enforces this by accepting only sequential generations and by allowing
// retention to erase only the oldest. That is what makes "is this range fully
// covered" an O(1) test instead of a scan, and it is why a query can never
// return a partial union.
class HistoryStore {
public:
    // durableSink may be null (pure in-memory use, tests). When present it is
    // called before a delta becomes visible, so a persistence failure leaves the
    // history unchanged.
    explicit HistoryStore(const Geometry& geometry, SealSink* durableSink = nullptr);

    HistoryStore(const HistoryStore&) = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;
    HistoryStore(HistoryStore&&) = delete;
    HistoryStore& operator=(HistoryStore&&) = delete;

    const Geometry& geometry() const { return geometry_; }

    // SealSink contract. Must be called under the owning device's exclusive
    // lock. Rejects a generation that is not exactly newest + 1 with
    // Status::InvalidState rather than creating a hole.
    Status commitDelta(Generation generation, const DirtyBitmap& delta);

    Status delta(Generation generation, SealedDelta& out) const;
    std::vector<Generation> generations() const;

    Generation oldestGeneration() const; // 0 when empty
    Generation newestGeneration() const; // 0 when empty
    std::uint64_t deltaCount() const;

    // Union of every delta in (fromExclusive, toInclusive].
    //
    // Returns Status::NotFound, never a partial result, when:
    //   - toInclusive has not been sealed yet, or
    //   - the retained history does not cover the whole interval.
    // Returns InvalidArgument when toInclusive < fromExclusive.
    // An empty interval (fromExclusive == toInclusive) is Ok and empty.
    Status unionRange(Generation fromExclusive,
                      Generation toInclusive,
                      DirtyBitmap& out) const;

    // --- checkpoint grid -------------------------------------------------

    std::uint32_t checkpointInterval() const;
    std::vector<Generation> checkpoints() const;

    // OR of every delta in [firstGeneration, generation]. Only generations on
    // the checkpoint grid exist; Status::NotFound otherwise.
    //
    // This is an analysis structure, not a query accelerator: it cannot answer
    // "changed since C" for any C other than the baseline. Use unionRange for
    // incremental answers.
    Status cumulative(Generation generation, SealedDelta& out) const;

    // Changing the interval discards existing checkpoints. Correctness is
    // unaffected; only acceleration is lost until they are rebuilt.
    void setCheckpointInterval(std::uint32_t interval);

    // Discards and recomputes the checkpoint grid from the retained deltas.
    // Used by tests and by the persistence layer after recovery.
    Status rebuildCheckpoints();

    // --- retention -------------------------------------------------------

    // Removes the given generation from history. Only the oldest generation may
    // be erased, because anything else would create a hole that no query can
    // answer and no consumer can reason about.
    //
    // Callers (the garbage collector) must have durably invalidated any consumer
    // that still needed it *before* calling this.
    bool erase(Generation generation);

private:
    // Requires the unique lock.
    Status rebuildLocked();
    Status unionRangeLocked(Generation fromExclusive,
                            Generation toInclusive,
                            DirtyBitmap& out) const;
    Status orDeltaIntoLocked(Generation generation, DirtyBitmap& accumulator) const;

    Geometry geometry_;
    SealSink* durableSink_;

    mutable std::shared_mutex mutex_;
    std::map<Generation, SealedDelta> deltas_;
    std::map<Generation, SealedDelta> checkpoints_;

    SealedDelta cumulative_;   // OR of every retained-original delta up to newest_
    bool cumulativeValid_ = false;

    Generation firstGeneration_ = 0; // first generation ever committed, never reset
    Generation oldest_ = 0;
    Generation newest_ = 0;
    std::uint32_t interval_ = 16;
};

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_HISTORY_H
