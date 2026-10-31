// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_GENERATION_H
#define BLOCKCHRONICLE_GENERATION_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <vector>

#include "blockchronicle/bitmap.h"
#include "blockchronicle/types.h"

namespace blockchronicle {

// A sealed generation's delta. Immutable by type: no caller can modify history.
using SealedDelta = std::shared_ptr<const DirtyBitmap>;

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

// Persistence hook for the seal protocol.
//
// Call contract: invoked by GenerationManager while the device's exclusive lock
// is held. It must make the delta durable before returning Ok. It must not call
// back into the same GenerationManager (that would self-deadlock).
//
// Returning anything other than Ok aborts the seal completely: nothing is
// published, the active generation keeps its bits and keeps collecting writes.
class SealSink {
public:
    virtual ~SealSink() = default;
    virtual Status commitDelta(Generation generation, const DirtyBitmap& delta) = 0;
};

// Owns the active generation, the seal protocol, and the visible sealed history
// for one device.
//
// Invariants:
//   I1  exactly one generation is active
//   I2  generation numbers are never reused
//   I3  a generation appears in history only after its SealSink returned Ok
//   I4  no write is ever attributed to a sealed generation
//   I5  history deltas are immutable
//
// Locking: stateMutex_ is held shared while marking and while reading metadata,
// and exclusively for seal/erase. Sealing therefore waits for in-flight marks,
// and a mark that begins during a seal is blocked until the new generation is
// installed. That is the whole correctness argument for I4; there is no
// per-generation lock and no lock-free rotation.
class GenerationManager {
public:
    GenerationManager(const Geometry& geometry, Generation startGeneration, SealSink& sink);

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

    // Sealed delta lookup. Returns NotFound for an unsealed or already reclaimed
    // generation; never fabricates an empty bitmap.
    Status delta(Generation generation, SealedDelta& out) const;

    // Ascending list of visible sealed generations.
    std::vector<Generation> sealedGenerations() const;

    // Retention support: removes a sealed generation. Returns false if absent.
    // Callers are responsible for proving no consumer still needs it; this class
    // deliberately has no opinion about consumers.
    bool eraseGeneration(Generation generation);

private:
    Status sealLocked();

    Geometry geometry_;
    SealSink& sink_;
    mutable std::shared_mutex stateMutex_;
    std::unique_ptr<ActiveBitmap> active_;
    std::map<Generation, SealedDelta> history_;
    Generation lastSealed_ = 0;
};

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_GENERATION_H
