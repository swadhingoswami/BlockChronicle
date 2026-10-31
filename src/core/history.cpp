// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/history.h"

#include <utility>

namespace blockchronicle {

HistoryStore::HistoryStore(const Geometry& geometry, SealSink* durableSink)
    : geometry_(geometry)
    , durableSink_(durableSink)
{
}

Status HistoryStore::commitDelta(Generation generation, const DirtyBitmap& delta)
{
    if (!delta.geometry().equals(geometry_)) {
        return Status::InvalidArgument;
    }

    std::unique_lock<std::shared_mutex> guard(mutex_);

    const bool empty = deltas_.empty();
    if (!empty && generation != newest_ + 1u) {
        // Refuse to create a hole. Contiguity is what makes coverage checks O(1)
        // and makes a partial union impossible to return.
        return Status::InvalidState;
    }

    // Durable before visible.
    if (durableSink_ != nullptr) {
        const Status status = durableSink_->commitDelta(generation, delta);
        if (status != Status::Ok) {
            return status;
        }
    }

    if (empty) {
        firstGeneration_ = generation;
        oldest_ = generation;
        newest_ = generation;
        checkpoints_.clear();
        cumulative_ = std::make_shared<const DirtyBitmap>(geometry_);
        cumulativeValid_ = true;
    } else {
        if (!cumulativeValid_) {
            const Status status = rebuildLocked();
            if (status != Status::Ok) {
                return status;
            }
        }
        newest_ = generation;
    }

    const SealedDelta stored = std::make_shared<const DirtyBitmap>(delta);
    deltas_[generation] = stored;

    DirtyBitmap merged(geometry_);
    const Status mergeStatus = DirtyBitmap::bitwiseOr(*cumulative_, delta, merged);
    if (mergeStatus != Status::Ok) {
        // Unreachable: geometry was validated above.
        return mergeStatus;
    }
    cumulative_ = std::make_shared<const DirtyBitmap>(std::move(merged));

    const Generation ordinal = generation - firstGeneration_ + 1u;
    if (interval_ != 0u && (ordinal % interval_) == 0u) {
        checkpoints_[generation] = cumulative_;
    }

    return Status::Ok;
}

Status HistoryStore::delta(Generation generation, SealedDelta& out) const
{
    std::shared_lock<std::shared_mutex> guard(mutex_);
    const std::map<Generation, SealedDelta>::const_iterator it = deltas_.find(generation);
    if (it == deltas_.end()) {
        return Status::NotFound;
    }
    out = it->second;
    return Status::Ok;
}

std::vector<Generation> HistoryStore::generations() const
{
    std::shared_lock<std::shared_mutex> guard(mutex_);
    std::vector<Generation> result;
    result.reserve(deltas_.size());
    for (std::map<Generation, SealedDelta>::const_iterator it = deltas_.begin();
         it != deltas_.end();
         ++it) {
        result.push_back(it->first);
    }
    return result;
}

Generation HistoryStore::oldestGeneration() const
{
    std::shared_lock<std::shared_mutex> guard(mutex_);
    return oldest_;
}

Generation HistoryStore::newestGeneration() const
{
    std::shared_lock<std::shared_mutex> guard(mutex_);
    return newest_;
}

std::uint64_t HistoryStore::deltaCount() const
{
    std::shared_lock<std::shared_mutex> guard(mutex_);
    return static_cast<std::uint64_t>(deltas_.size());
}

// ---------------------------------------------------------------------------
// Union queries
// ---------------------------------------------------------------------------

Status HistoryStore::orDeltaIntoLocked(Generation generation, DirtyBitmap& accumulator) const
{
    const std::map<Generation, SealedDelta>::const_iterator it = deltas_.find(generation);
    if (it == deltas_.end()) {
        return Status::NotFound;
    }
    // Output aliasing an input is supported and tested.
    return DirtyBitmap::bitwiseOr(accumulator, *it->second, accumulator);
}

Status HistoryStore::unionRange(Generation fromExclusive,
                                Generation toInclusive,
                                DirtyBitmap& out) const
{
    std::shared_lock<std::shared_mutex> guard(mutex_);
    return unionRangeLocked(fromExclusive, toInclusive, out);
}

Status HistoryStore::unionRangeLocked(Generation fromExclusive,
                                      Generation toInclusive,
                                      DirtyBitmap& out) const
{
    if (toInclusive < fromExclusive) {
        return Status::InvalidArgument;
    }

    DirtyBitmap result(geometry_);

    if (toInclusive == fromExclusive) {
        out = std::move(result);
        return Status::Ok;
    }

    if (deltas_.empty() || toInclusive > newest_) {
        // The upper boundary is not sealed, so no complete answer exists.
        return Status::NotFound;
    }

    // (fromExclusive, toInclusive] must be entirely inside retained history.
    if (fromExclusive + 1u < oldest_) {
        return Status::NotFound;
    }

    // The definition: OR of every delta in the interval.
    //
    // There is deliberately no cumulative shortcut here. `cumulative(to) AND NOT
    // cumulative(from)` computes "changed in (from, to] AND never changed before
    // from", which silently omits every region that changed on both sides of the
    // cursor. See HistoryStore.CumulativeDifferenceIsNotTheIncrementalUnion and
    // docs/bitmap-history.md section 3.
    //
    // Cost is O(number of generations in the interval). Bounding that cost is a
    // per-consumer concern: the consumer's pending bitmap is maintained
    // incrementally at seal time (Phase 4), which is the only correct
    // acceleration because a device-wide cumulative cannot represent "changed
    // since C" for an arbitrary C.
    for (Generation generation = fromExclusive + 1u; generation <= toInclusive;
         ++generation) {
        const Status status = orDeltaIntoLocked(generation, result);
        if (status != Status::Ok) {
            return status;
        }
    }

    out = std::move(result);
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Checkpoints
// ---------------------------------------------------------------------------

std::uint32_t HistoryStore::checkpointInterval() const
{
    std::shared_lock<std::shared_mutex> guard(mutex_);
    return interval_;
}

std::vector<Generation> HistoryStore::checkpoints() const
{
    std::shared_lock<std::shared_mutex> guard(mutex_);
    std::vector<Generation> result;
    result.reserve(checkpoints_.size());
    for (std::map<Generation, SealedDelta>::const_iterator it = checkpoints_.begin();
         it != checkpoints_.end();
         ++it) {
        result.push_back(it->first);
    }
    return result;
}

Status HistoryStore::cumulative(Generation generation, SealedDelta& out) const
{
    std::shared_lock<std::shared_mutex> guard(mutex_);
    const std::map<Generation, SealedDelta>::const_iterator it = checkpoints_.find(generation);
    if (it == checkpoints_.end()) {
        return Status::NotFound;
    }
    out = it->second;
    return Status::Ok;
}

void HistoryStore::setCheckpointInterval(std::uint32_t interval)
{
    std::unique_lock<std::shared_mutex> guard(mutex_);
    interval_ = interval;
    checkpoints_.clear();
    cumulativeValid_ = false;
}

Status HistoryStore::rebuildCheckpoints()
{
    std::unique_lock<std::shared_mutex> guard(mutex_);
    if (deltas_.empty()) {
        checkpoints_.clear();
        cumulative_ = std::make_shared<const DirtyBitmap>(geometry_);
        cumulativeValid_ = true;
        return Status::Ok;
    }
    return rebuildLocked();
}

Status HistoryStore::rebuildLocked()
{
    checkpoints_.clear();

    DirtyBitmap accumulator(geometry_);

    for (Generation generation = oldest_; generation <= newest_; ++generation) {
        const std::map<Generation, SealedDelta>::const_iterator it = deltas_.find(generation);
        if (it == deltas_.end()) {
            cumulativeValid_ = false;
            return Status::NotFound;
        }

        const Status status = DirtyBitmap::bitwiseOr(accumulator, *it->second, accumulator);
        if (status != Status::Ok) {
            cumulativeValid_ = false;
            return status;
        }

        const Generation ordinal = generation - firstGeneration_ + 1u;
        if (interval_ != 0u && (ordinal % interval_) == 0u) {
            checkpoints_[generation] = std::make_shared<const DirtyBitmap>(accumulator);
        }
    }

    cumulative_ = std::make_shared<const DirtyBitmap>(std::move(accumulator));
    cumulativeValid_ = true;
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Retention
// ---------------------------------------------------------------------------

bool HistoryStore::erase(Generation generation)
{
    std::unique_lock<std::shared_mutex> guard(mutex_);

    if (deltas_.empty() || generation != oldest_) {
        return false;
    }

    deltas_.erase(generation);

    // Every checkpoint contains the erased delta, and the cumulative baseline
    // has moved. Drop the grid and rebuild lazily at the next commit or query.
    checkpoints_.clear();
    cumulativeValid_ = false;

    if (deltas_.empty()) {
        oldest_ = 0;
        newest_ = 0;
    } else {
        oldest_ = deltas_.begin()->first;
    }
    return true;
}

} // namespace blockchronicle
