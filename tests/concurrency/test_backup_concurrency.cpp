// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Phase 4 concurrency test: several consumers take sessions and enumerate ranges
// while the device keeps sealing new generations.
//
// The assertion is exact in both directions: a session must contain every region
// changed up to its boundary (no false negatives) and nothing changed after it
// (no false positives). A missing lock around the pending-set publication shows
// up as one or the other, and TSan reports the race independently.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "blockchronicle/consumer.h"
#include "blockchronicle/generation.h"
#include "blockchronicle/history.h"
#include "support/status_printer.h"

namespace {

using namespace blockchronicle;

constexpr std::uint32_t k64K = 64u * 1024u;
constexpr std::uint32_t kRegions = 512;
constexpr Generation kLastGeneration = 200;

// Records the set of every region marked up to each sealed generation. Readers
// block until the generation they were given a boundary for has been published.
class Oracle {
public:
    void publish(Generation generation, const std::vector<std::uint32_t>& cumulative)
    {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            absolute_.resize(static_cast<std::size_t>(generation) + 1u);
            absolute_[static_cast<std::size_t>(generation)] = cumulative;
        }
        condition_.notify_all();
    }

    std::vector<std::uint32_t> waitFor(Generation generation)
    {
        std::unique_lock<std::mutex> guard(mutex_);
        while (absolute_.size() <= static_cast<std::size_t>(generation)) {
            condition_.wait(guard);
        }
        return absolute_[static_cast<std::size_t>(generation)];
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::vector<std::uint32_t>> absolute_;
};

std::set<std::uint32_t> expandRanges(const std::vector<DirtyRange>& ranges)
{
    std::set<std::uint32_t> regions;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const std::uint64_t first = ranges[i].offsetBytes / k64K;
        const std::uint64_t last =
            (ranges[i].offsetBytes + ranges[i].lengthBytes - 1u) / k64K;
        for (std::uint64_t region = first; region <= last; ++region) {
            regions.insert(static_cast<std::uint32_t>(region));
        }
    }
    return regions;
}

TEST(BackupConcurrency, SessionsSeeExactlyTheirOwnBoundaryUnderConcurrentSeals)
{
    Geometry geometry;
    ASSERT_EQ(Geometry::make(kRegions * k64K, k64K, geometry), Status::Ok);

    HistoryStore history(geometry);
    ConsumerManager consumers(geometry, 1);
    GenerationManager manager(history, 1, &consumers);

    // Generation 1 is an empty baseline for every consumer.
    ASSERT_EQ(manager.seal(), Status::Ok);

    Oracle oracle;
    oracle.publish(1, std::vector<std::uint32_t>());

    constexpr int kConsumers = 4;
    constexpr int kIterations = 300;

    for (int i = 0; i < kConsumers; ++i) {
        const std::string id = "consumer-" + std::to_string(i);
        ASSERT_EQ(consumers.addConsumer(id), Status::Ok);

        BackupSession session;
        ASSERT_EQ(manager.beginBackup(id, session), Status::Ok);
        ASSERT_EQ(session.kind, BackupKind::Full);
        ASSERT_EQ(consumers.commit(session, 1), Status::Ok);
    }

    std::atomic<bool> deviceDone{false};
    std::atomic<int> failures{0};

    std::thread device([&]() {
        std::vector<std::uint32_t> cumulative;
        std::mt19937 rng(0x4B1D2026u);

        for (Generation generation = 2; generation <= kLastGeneration; ++generation) {
            for (int i = 0; i < 3; ++i) {
                const std::uint32_t region = rng() % kRegions;
                if (!manager.markRegion(region)) {
                    failures.fetch_add(1);
                }
                bool present = false;
                for (std::size_t j = 0; j < cumulative.size(); ++j) {
                    if (cumulative[j] == region) {
                        present = true;
                        break;
                    }
                }
                if (!present) {
                    cumulative.push_back(region);
                }
            }
            if (manager.seal() != Status::Ok) {
                failures.fetch_add(1);
            }
            oracle.publish(generation, cumulative);
        }
        deviceDone.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    readers.reserve(kConsumers);

    for (int i = 0; i < kConsumers; ++i) {
        const std::string id = "consumer-" + std::to_string(i);
        readers.push_back(std::thread([&, id]() {
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                BackupSession session;
                const Status began = manager.beginBackup(id, session);
                if (began != Status::Ok) {
                    failures.fetch_add(1);
                    continue;
                }

                std::vector<DirtyRange> ranges;
                if (getDirtyRanges(session, 8192, ranges) != Status::Ok) {
                    failures.fetch_add(1);
                }

                const std::vector<std::uint32_t> expectedRegions =
                    oracle.waitFor(session.boundary);
                const std::set<std::uint32_t> expected(expectedRegions.begin(),
                                                        expectedRegions.end());

                const std::set<std::uint32_t> actual = expandRanges(ranges);
                if (actual != expected) {
                    failures.fetch_add(1);
                }

                if (consumers.abort(session) != Status::Ok) {
                    failures.fetch_add(1);
                }
            }
        }));
    }

    for (std::size_t i = 0; i < readers.size(); ++i) {
        readers[i].join();
    }
    device.join();

    EXPECT_TRUE(deviceDone.load(std::memory_order_acquire));
    EXPECT_EQ(failures.load(), 0);
}

} // namespace
