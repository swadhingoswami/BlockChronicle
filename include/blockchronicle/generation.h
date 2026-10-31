// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_GENERATION_H
#define BLOCKCHRONICLE_GENERATION_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <vector>

#include "blockchronicle/bitmap.h"
#include "blockchronicle/consumer.h"
#include "blockchronicle/seal.h"
#include "blockchronicle/types.h"

namespace blockchronicle {

class HistoryStore;

// Storage for the generation currently collecting writes.
//
// Marking is lock-free at the word level: one fetch_or per touched word. The
// memory_order_relaxed operations are made visible to a sealing thread by the
// shared_mutex handoff in GenerationManager, which is a release/acquire
// synchronisation. See docs/concurrency.md section 2.
//
// ActiveBitmap is itself unsynchronised with respect to freeze(): the caller
// must guarantee no writer is active. GenerationManager does this by holding the
// exclusive lock during freeze.
class ActiveBitmap {
public:
    ActiveBitmap(const Geometry& geometry, Generation generation);

    ActiveBitmap(const ActiveBitmap&) = delete;
    ActiveBitmap& operator=(const ActiveBitmap&) = delete;
    ActiveBitmap(ActiveBitmap&&) = delete;
    ActiveBitmap& operator=(ActiveBitmap&&) = delete;

    Generation generation() const { return generation_; }
    const Geometry& geometry() const { return geometry_; }
    std::size_t wordCount() const { return wordCount_; }

    bool markRegion(std::uint32_t region);
    bool markRegionRange(std::uint32_t firstRegion, std::uint32_t lastRegion);
    Status markWrite(std::uint64_t offsetBytes, std::uint64_t lengthBytes);

    bool test(std::uint32_t region) const;
    std::uint64_t countDirty() const;
    bool any() const;

    // Copies the current bits into out. Requires that writers are excluded.
    void freeze(DirtyBitmap& out) const;

private:
    Geometry geometry_;
    Generation generation_;
    std::size_t wordCount_ = 0;
    std::unique_ptr<std::atomic<Word>[]> words_;
};

// Owns the active generation and the seal protocol for one device. Sealed
// deltas are published into a HistoryStore, which owns retention and queries.
//
// Invariants:
//   I1  exactly one generation is active
//   I2  generation numbers are never reused
//   I3  a generation becomes visible only after its HistoryStore commit returned Ok
//   I4  no write is ever attributed to a sealed generation
//   I5  history deltas are immutable
//
// Locking: stateMutex_ is held shared while marking and while reading metadata,
// and exclusively for seal/erase. Sealing therefore waits for in-flight marks,
// and a mark that begins during a seal is blocked until the new generation is
// installed. Lock order is stateMutex_ then HistoryStore::mutex_; nothing takes
// them in the other direction.
class GenerationManager {
public:
    // The device geometry is taken from the history store, so the two cannot
    // disagree. `consumers` may be null; consumer-facing methods then return
    // Status::Unsupported.
    GenerationManager(HistoryStore& history,
                      Generation startGeneration,
                      ConsumerManager* consumers = nullptr);

    GenerationManager(const GenerationManager&) = delete;
    GenerationManager& operator=(const GenerationManager&) = delete;
    GenerationManager(GenerationManager&&) = delete;
    GenerationManager& operator=(GenerationManager&&) = delete;

    // Marking. Takes the shared lock, so it cannot interleave with seal().
    bool markRegion(std::uint32_t region);
    Status markWrite(std::uint64_t offsetBytes, std::uint64_t lengthBytes);

    // Closes the active generation and publishes its delta.
    // On failure the active generation, its bits, and the history are unchanged.
    Status seal();

    // Seals only if the active generation holds at least `threshold` dirty
    // regions. Returns Ok without sealing otherwise. Used by the maintenance
    // thread to bound delta size.
    Status sealIfDirtyAtLeast(std::uint64_t threshold);

    Generation activeGeneration() const;
    Generation lastSealedGeneration() const; // 0 when nothing has been sealed
    std::uint64_t activeDirtyCount() const;

    // Starts a backup session at the current sealed boundary.
    //
    // This is the only place a boundary and a pending set are captured, and both
    // happen under the shared device lock, so a seal cannot interleave. That is
    // the invariant proved in docs/consistency-model.md section 4: the boundary
    // is the generation that was active when the snapshot was taken.
    Status beginBackup(const ConsumerId& consumer, BackupSession& out);

    ConsumerManager* consumers() { return consumers_; }
    const ConsumerManager* consumers() const { return consumers_; }

    // Delegates to the history store.
    Status delta(Generation generation, SealedDelta& out) const;
    std::vector<Generation> sealedGenerations() const;
    bool eraseGeneration(Generation generation);

private:
    Status sealLocked();

    HistoryStore& history_;
    ConsumerManager* consumers_;
    mutable std::shared_mutex stateMutex_;
    std::unique_ptr<ActiveBitmap> active_;
};

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_GENERATION_H
