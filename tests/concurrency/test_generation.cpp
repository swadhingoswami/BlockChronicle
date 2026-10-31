// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Phase 2 concurrency acceptance tests.
//
// The tests that matter here are the two that can fail silently in production:
//
//   MarkInFlightDuringSealIsNotLost
//       A mark that begins while a seal is committing must land in the NEXT
//       generation. If the shared/exclusive protocol were missing, the mark would
//       mutate the frozen bitmap after it was copied and before it was published,
//       and the region would appear in no generation at all.
//
//   NoMarkLostAcrossConcurrentSeals
//       Every successful mark, over 8 writers and many seals, must appear in
//       exactly the union of sealed deltas. Nothing lost, nothing invented.
//
// Both assert on final state, not on timing. The handshakes only establish
// happens-before; a slow or fast machine cannot flip the result.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "blockchronicle/generation.h"
#include "support/status_printer.h"

namespace {

using namespace blockchronicle;

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

// Records committed deltas and can be told to fail the next N commits.
class RecordingSink : public SealSink {
public:
    Status commitDelta(Generation generation, const DirtyBitmap&) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (failuresRemaining_ > 0) {
            --failuresRemaining_;
            return Status::IoError;
        }
        committed.push_back(generation);
        return Status::Ok;
    }

    void failNextCommit()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        ++failuresRemaining_;
    }

    std::size_t commitCount() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return committed.size();
    }

    std::vector<Generation> committed;

private:
    mutable std::mutex mutex_;
    int failuresRemaining_ = 0;
};

// Blocks inside commitDelta until the test thread has attempted a mark, then
// waits long enough that an unlocked implementation would certainly have
// finished the erroneous mutation.
class HandshakeSink : public SealSink {
public:
    Status commitDelta(Generation, const DirtyBitmap&) override
    {
        sealEntered.set_value();
        markStarted.get_future().wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return Status::Ok;
    }

    std::promise<void> sealEntered;
    std::promise<void> markStarted;
};

DirtyBitmap unionOfSealed(const GenerationManager& manager, const Geometry& geometry)
{
    DirtyBitmap result(geometry);
    const std::vector<Generation> generations = manager.sealedGenerations();

    for (std::size_t i = 0; i < generations.size(); ++i) {
        SealedDelta delta;
        EXPECT_EQ(manager.delta(generations[i], delta), Status::Ok);
        DirtyBitmap merged(result.geometry());
        EXPECT_EQ(DirtyBitmap::bitwiseOr(result, *delta, merged), Status::Ok);
        result = std::move(merged);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Basic lifecycle
// ---------------------------------------------------------------------------

TEST(GenerationManager, SealPublishesTheFrozenDelta)
{
    RecordingSink sink;
    GenerationManager manager(makeGeometry(64 * k64K), 1, sink);

    ASSERT_EQ(manager.markWrite(10 * k64K, 1), Status::Ok);
    ASSERT_EQ(manager.markWrite(20 * k64K, 1), Status::Ok);
    EXPECT_EQ(manager.activeGeneration(), 1u);
    EXPECT_EQ(manager.activeDirtyCount(), 2u);

    ASSERT_EQ(manager.seal(), Status::Ok);

    EXPECT_EQ(manager.activeGeneration(), 2u);
    EXPECT_EQ(manager.lastSealedGeneration(), 1u);
    EXPECT_EQ(manager.activeDirtyCount(), 0u);

    SealedDelta delta;
    ASSERT_EQ(manager.delta(1, delta), Status::Ok);
    ASSERT_TRUE(delta != nullptr);
    EXPECT_EQ(dirtyRegions(*delta), (std::vector<std::uint32_t>{10, 20}));
}

TEST(GenerationManager, WritesAfterASealGoToTheNextGeneration)
{
    RecordingSink sink;
    GenerationManager manager(makeGeometry(64 * k64K), 1, sink);

    ASSERT_EQ(manager.markWrite(10 * k64K, 1), Status::Ok);
    ASSERT_EQ(manager.seal(), Status::Ok);
    ASSERT_EQ(manager.markWrite(30 * k64K, 1), Status::Ok);
    ASSERT_EQ(manager.markWrite(40 * k64K, 1), Status::Ok);

    SealedDelta first;
    ASSERT_EQ(manager.delta(1, first), Status::Ok);
    EXPECT_EQ(dirtyRegions(*first), (std::vector<std::uint32_t>{10}));

    SealedDelta second;
    EXPECT_EQ(manager.delta(2, second), Status::NotFound)
        << "an unsealed generation must never be readable";

    ASSERT_EQ(manager.seal(), Status::Ok);
    ASSERT_EQ(manager.delta(2, second), Status::Ok);
    EXPECT_EQ(dirtyRegions(*second), (std::vector<std::uint32_t>{30, 40}));
}

TEST(GenerationManager, EmptyGenerationStillSeals)
{
    RecordingSink sink;
    GenerationManager manager(makeGeometry(64 * k64K), 7, sink);

    ASSERT_EQ(manager.seal(), Status::Ok);

    SealedDelta delta;
    ASSERT_EQ(manager.delta(7, delta), Status::Ok);
    EXPECT_TRUE(delta->none());
    EXPECT_EQ(manager.activeGeneration(), 8u);
    EXPECT_EQ(manager.lastSealedGeneration(), 7u);
}

TEST(GenerationManager, DeltaForUnknownGenerationIsNotFound)
{
    RecordingSink sink;
    GenerationManager manager(makeGeometry(64 * k64K), 1, sink);

    SealedDelta delta;
    EXPECT_EQ(manager.delta(1, delta), Status::NotFound);
    EXPECT_EQ(manager.delta(999, delta), Status::NotFound);
    EXPECT_TRUE(delta == nullptr);
}

TEST(GenerationManager, SealedDeltaIsImmutableByType)
{
    RecordingSink sink;
    GenerationManager manager(makeGeometry(64 * k64K), 1, sink);
    ASSERT_EQ(manager.seal(), Status::Ok);

    SealedDelta delta;
    ASSERT_EQ(manager.delta(1, delta), Status::Ok);

    using Pointee = std::remove_reference<decltype(*delta)>::type;
    static_assert(std::is_const<Pointee>::value,
                  "a sealed delta must not be mutable through its handle");
}

// ---------------------------------------------------------------------------
// Failure handling: the brief's Phase 2 acceptance scenario
// ---------------------------------------------------------------------------

TEST(GenerationManager, FailedSealLosesNothingAndRetrySucceeds)
{
    const Geometry geometry = makeGeometry(64 * k64K);
    RecordingSink sink;
    GenerationManager manager(geometry, 1, sink);

    // 1-2. Mark, then seal the first generation.
    ASSERT_EQ(manager.markWrite(10 * k64K, 1), Status::Ok);
    ASSERT_EQ(manager.markWrite(20 * k64K, 1), Status::Ok);
    ASSERT_EQ(manager.seal(), Status::Ok);

    // 3. New writes while the next generation is active.
    ASSERT_EQ(manager.markWrite(30 * k64K, 1), Status::Ok);
    ASSERT_EQ(manager.markWrite(40 * k64K, 1), Status::Ok);

    // 4-6. The seal fails (simulated I/O error). Nothing is published and the
    // active generation keeps its bits.
    sink.failNextCommit();
    EXPECT_EQ(manager.seal(), Status::IoError);

    EXPECT_EQ(manager.lastSealedGeneration(), 1u);
    EXPECT_EQ(manager.activeGeneration(), 2u);
    EXPECT_EQ(manager.activeDirtyCount(), 2u);

    SealedDelta unsealed;
    EXPECT_EQ(manager.delta(2, unsealed), Status::NotFound);

    // 7-8. Retry succeeds and contains everything from the failed attempt.
    ASSERT_EQ(manager.seal(), Status::Ok);
    SealedDelta second;
    ASSERT_EQ(manager.delta(2, second), Status::Ok);
    EXPECT_EQ(dirtyRegions(*second), (std::vector<std::uint32_t>{30, 40}));

    // 9. Nothing lost across the whole sequence.
    const DirtyBitmap all = unionOfSealed(manager, geometry);
    EXPECT_EQ(dirtyRegions(all), (std::vector<std::uint32_t>{10, 20, 30, 40}));
}

TEST(GenerationManager, MarkAfterFailedSealIsNotMisattributed)
{
    RecordingSink sink;
    GenerationManager manager(makeGeometry(64 * k64K), 1, sink);

    ASSERT_EQ(manager.markWrite(5 * k64K, 1), Status::Ok);
    sink.failNextCommit();
    ASSERT_EQ(manager.seal(), Status::IoError);

    // The generation did not advance, so this belongs to generation 1 too.
    ASSERT_EQ(manager.markWrite(6 * k64K, 1), Status::Ok);
    ASSERT_EQ(manager.seal(), Status::Ok);

    SealedDelta delta;
    ASSERT_EQ(manager.delta(1, delta), Status::Ok);
    EXPECT_EQ(dirtyRegions(*delta), (std::vector<std::uint32_t>{5, 6}));
}

// ---------------------------------------------------------------------------
// The seal/mark exclusion protocol
// ---------------------------------------------------------------------------

TEST(GenerationManager, MarkInFlightDuringSealIsNotLost)
{
    HandshakeSink sink;
    GenerationManager manager(makeGeometry(64 * k64K), 1, sink);

    std::atomic<Status> sealStatus{Status::InvalidState};
    std::thread sealer([&manager, &sealStatus]() {
        sealStatus.store(manager.seal(), std::memory_order_release);
    });

    // Wait until commitDelta is running, i.e. the exclusive lock is held and the
    // frozen copy has been taken.
    sink.sealEntered.get_future().wait();

    std::atomic<bool> markReturned{false};
    std::thread writer([&manager, &sink, &markReturned]() {
        sink.markStarted.set_value(); // about to call markRegion
        manager.markRegion(3);
        markReturned.store(true, std::memory_order_release);
    });

    writer.join();
    sealer.join();

    EXPECT_EQ(sealStatus.load(std::memory_order_acquire), Status::Ok);
    EXPECT_TRUE(markReturned.load(std::memory_order_acquire));

    SealedDelta first;
    ASSERT_EQ(manager.delta(1, first), Status::Ok);
    EXPECT_FALSE(first->test(3)) << "a mark made during the seal must not be in the "
                                    "closing generation's published delta";

    EXPECT_EQ(manager.activeDirtyCount(), 1u)
        << "the mark must have landed in the newly active generation";
    EXPECT_TRUE(manager.markRegion(3)); // idempotent, still one dirty region
    EXPECT_EQ(manager.activeDirtyCount(), 1u);
}

TEST(GenerationManager, ConcurrentMarksOnTheSameRegionAreIdempotent)
{
    RecordingSink sink;
    GenerationManager manager(makeGeometry(64 * k64K), 1, sink);

    constexpr int kThreads = 8;
    constexpr int kMarksPerThread = 20000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.push_back(std::thread([&manager]() {
            for (int i = 0; i < kMarksPerThread; ++i) {
                manager.markRegion(5);
            }
        }));
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    EXPECT_EQ(manager.activeDirtyCount(), 1u);
}

TEST(GenerationManager, NoMarkLostAcrossConcurrentSeals)
{
    // 1024 regions = 64 MiB at the default granularity.
    const Geometry geometry = makeGeometry(1024 * k64K);
    RecordingSink sink;
    GenerationManager manager(geometry, 1, sink);

    constexpr int kWriterThreads = 8;
    constexpr int kMarksPerWriter = 5000;

    std::vector<std::thread> writers;
    std::vector<std::vector<std::uint32_t>> recorded(kWriterThreads);
    writers.reserve(kWriterThreads);

    for (int t = 0; t < kWriterThreads; ++t) {
        writers.push_back(std::thread([&manager, &recorded, t]() {
            std::mt19937 rng(static_cast<unsigned int>(t) * 7919u + 13u);
            recorded[static_cast<std::size_t>(t)].reserve(kMarksPerWriter);
            for (int i = 0; i < kMarksPerWriter; ++i) {
                const std::uint32_t region = rng() % 1024u;
                if (manager.markRegion(region)) {
                    recorded[static_cast<std::size_t>(t)].push_back(region);
                }
            }
        }));
    }

    // Seal aggressively while the writers are running: this is the interleaving
    // that exposes a missing lock.
    std::atomic<bool> writersDone{false};
    std::thread sealer([&manager, &writersDone]() {
        int seals = 0;
        while (!writersDone.load(std::memory_order_acquire) && seals < 2000) {
            EXPECT_EQ(manager.seal(), Status::Ok);
            ++seals;
        }
    });

    for (std::size_t i = 0; i < writers.size(); ++i) {
        writers[i].join();
    }
    writersDone.store(true, std::memory_order_release);
    sealer.join();

    ASSERT_EQ(manager.seal(), Status::Ok); // flush the final active generation

    std::set<std::uint32_t> oracle;
    for (std::size_t t = 0; t < recorded.size(); ++t) {
        for (std::size_t i = 0; i < recorded[t].size(); ++i) {
            oracle.insert(recorded[t][i]);
        }
    }

    const DirtyBitmap all = unionOfSealed(manager, geometry);
    std::vector<std::uint32_t> unionRegions = dirtyRegions(all);

    EXPECT_EQ(unionRegions.size(), oracle.size())
        << "the union of sealed deltas must contain exactly the marked regions";

    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < unionRegions.size(); ++i) {
        if (oracle.count(unionRegions[i]) == 0u) {
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u) << "a region appeared in history that was never marked";

    std::size_t missing = 0;
    for (std::set<std::uint32_t>::const_iterator it = oracle.begin(); it != oracle.end(); ++it) {
        if (!all.test(*it)) {
            ++missing;
        }
    }
    EXPECT_EQ(missing, 0u) << "a marked region was lost across a seal";
    EXPECT_EQ(missing + mismatches, 0u);
}

// ---------------------------------------------------------------------------
// Periodic sealing policy
// ---------------------------------------------------------------------------

TEST(GenerationManager, SealIfDirtyAtLeastRespectsTheThreshold)
{
    RecordingSink sink;
    GenerationManager manager(makeGeometry(64 * k64K), 1, sink);

    ASSERT_TRUE(manager.markRegion(1));
    EXPECT_EQ(manager.sealIfDirtyAtLeast(3), Status::Ok);
    EXPECT_EQ(manager.lastSealedGeneration(), 0u) << "below threshold: no seal";

    ASSERT_TRUE(manager.markRegion(2));
    ASSERT_TRUE(manager.markRegion(3));
    EXPECT_EQ(manager.sealIfDirtyAtLeast(3), Status::Ok);
    EXPECT_EQ(manager.lastSealedGeneration(), 1u);
    EXPECT_EQ(manager.activeGeneration(), 2u);

    ASSERT_TRUE(manager.markRegion(4));
    EXPECT_EQ(manager.sealIfDirtyAtLeast(3), Status::Ok);
    EXPECT_EQ(manager.lastSealedGeneration(), 1u) << "one region is below the threshold";
}

TEST(GenerationManager, EraseGenerationRemovesOnlyThatDelta)
{
    RecordingSink sink;
    GenerationManager manager(makeGeometry(64 * k64K), 1, sink);
    ASSERT_EQ(manager.seal(), Status::Ok);
    ASSERT_EQ(manager.seal(), Status::Ok);

    EXPECT_TRUE(manager.eraseGeneration(1));
    EXPECT_FALSE(manager.eraseGeneration(1));

    SealedDelta delta;
    EXPECT_EQ(manager.delta(1, delta), Status::NotFound);
    EXPECT_EQ(manager.delta(2, delta), Status::Ok);
    EXPECT_EQ(manager.sealedGenerations(), (std::vector<Generation>{2}));
}

} // namespace
