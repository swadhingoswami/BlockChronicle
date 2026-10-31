// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Baseline measurements for the simulator backend. Numbers from one machine are
// not a specification; they are the starting point the project promised to
// establish before inventing performance targets.

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <unistd.h>

#include "blockchronicle/simulator.h"

namespace {

using namespace blockchronicle;

constexpr std::uint32_t k64K = 64u * 1024u;

double nowMs()
{
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch())
        .count();
}

std::string temporaryRoot()
{
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "/tmp/blockchronicle-bench-%d",
                  static_cast<int>(::getpid()));
    std::string command = "rm -rf " + std::string(buffer) + " && mkdir -p " + buffer;
    const int ignored = std::system(command.c_str());
    (void)ignored;
    return std::string(buffer);
}

} // namespace

int main()
{
    const std::string root = temporaryRoot();
    const std::uint64_t capacity = 256ull * 1024u * 1024u; // 4096 regions

    NoCrashPoint crash;
    DeviceOptions options;
    options.maxConsumers = 32; // the benchmark uses 10 consumers plus a probe

    std::unique_ptr<SimulatorDevice> device;
    Status status = SimulatorDevice::create(root + "/meta", root + "/disk.img",
                                            MetadataIdentity::fromSeed(1), capacity,
                                            options, crash, device);
    if (status != Status::Ok) {
        std::fprintf(stderr, "create failed: %s\n", toString(status));
        return 1;
    }

    std::printf("| Measurement | Result |\n");
    std::printf("|---|---|\n");
    std::printf("| Device capacity | %llu MiB |\n",
                static_cast<unsigned long long>(capacity / (1024u * 1024u)));
    std::printf("| Regions at 64 KiB | %u |\n", device->geometry().regionCount());

    // --- 1. Journaled write throughput -----------------------------------
    constexpr int kWrites = 200;
    std::vector<std::uint8_t> chunk(64u * 1024u, 0x5Au);
    std::mt19937 rng(7);

    double start = nowMs();
    for (int i = 0; i < kWrites; ++i) {
        const std::uint64_t offset =
            static_cast<std::uint64_t>(rng() % device->geometry().regionCount()) * k64K;
        if (device->write(offset, k64K, chunk.data()) != Status::Ok) {
            std::fprintf(stderr, "write failed\n");
            return 1;
        }
    }
    double elapsed = nowMs() - start;
    std::printf("| Marked 64 KiB writes (journaled, fsync each) | %.0f ops/s, %.3f ms/op |\n",
                kWrites * 1000.0 / elapsed, elapsed / kWrites);

    // --- 2. Seal cost ----------------------------------------------------
    constexpr int kSeals = 50;
    start = nowMs();
    for (int i = 0; i < kSeals; ++i) {
        if (device->seal() != Status::Ok) {
            std::fprintf(stderr, "seal failed\n");
            return 1;
        }
        const std::uint64_t offset =
            static_cast<std::uint64_t>(rng() % device->geometry().regionCount()) * k64K;
        if (device->write(offset, k64K, chunk.data()) != Status::Ok) {
            return 1;
        }
    }
    elapsed = nowMs() - start;
    std::printf("| Seal (4096-region bitmap) | %.3f ms/seal |\n", elapsed / kSeals);

    // --- 3. Range queries for ten consumers ------------------------------
    for (int i = 0; i < 10; ++i) {
        const std::string id = "consumer-" + std::to_string(i);
        device->addConsumer(id);
        BackupSession session;
        SnapshotHandle snapshot;
        if (device->beginBackup(id, session, snapshot) != Status::Ok) {
            return 1;
        }
        device->releaseSnapshot(snapshot);
        device->commitBackup(session, 1);
    }
    for (int i = 0; i < 200; ++i) {
        const std::uint64_t offset =
            static_cast<std::uint64_t>(rng() % device->geometry().regionCount()) * k64K;
        device->write(offset, k64K, chunk.data());
    }
    device->sealIfDirty();

    constexpr int kQueries = 200;
    std::uint64_t totalRanges = 0;
    double beginTotal = 0.0;
    double rangeTotal = 0.0;
    for (int i = 0; i < kQueries; ++i) {
        const std::string id = "consumer-" + std::to_string(i % 10);
        BackupSession session;
        SnapshotHandle snapshot;

        double moment = nowMs();
        if (device->beginBackup(id, session, snapshot) != Status::Ok) {
            return 1;
        }
        beginTotal += nowMs() - moment;

        std::vector<DirtyRange> ranges;
        moment = nowMs();
        if (device->getDirtyRanges(session, 1u << 20, ranges) != Status::Ok) {
            return 1;
        }
        rangeTotal += nowMs() - moment;

        totalRanges += ranges.size();
        device->releaseSnapshot(snapshot);
        device->abortBackup(session);
    }
    std::printf("| Begin backup (seal + snapshot copy, 256 MiB) | %.3f ms |\n",
                beginTotal / kQueries);
    std::printf("| Enumerate dirty ranges (10 consumers, %.0f ranges) | %.4f ms |\n",
                static_cast<double>(totalRanges) / kQueries, rangeTotal / kQueries);

    // --- 4. Checkpoint and recovery -------------------------------------
    start = nowMs();
    if (device->checkpoint() != Status::Ok) {
        return 1;
    }
    const double checkpointMs = nowMs() - start;
    std::printf("| Checkpoint (50 generations of history) | %.3f ms |\n", checkpointMs);

    if (device->close() != Status::Ok) {
        return 1;
    }
    device.reset();

    start = nowMs();
    status = SimulatorDevice::open(root + "/meta", root + "/disk.img",
                                   MetadataIdentity::fromSeed(1), options, crash, device);
    const double reopenMs = nowMs() - start;
    if (status != Status::Ok) {
        std::fprintf(stderr, "reopen failed: %s\n", toString(status));
        return 1;
    }
    std::printf("| Cold open (load history + replay) | %.3f ms |\n", reopenMs);

    // --- 5. Full snapshot copy ------------------------------------------
    device->addConsumer("snapshot-probe");
    BackupSession session;
    SnapshotHandle snapshot;
    start = nowMs();
    if (device->beginBackup("snapshot-probe", session, snapshot) != Status::Ok) {
        return 1;
    }
    const double snapshotMs = nowMs() - start;
    device->releaseSnapshot(snapshot);
    device->abortBackup(session);
    std::printf("| Snapshot copy of a %llu MiB image | %.3f ms (%.0f MiB/s) |\n",
                static_cast<unsigned long long>(capacity / (1024u * 1024u)), snapshotMs,
                (static_cast<double>(capacity) / (1024.0 * 1024.0)) / (snapshotMs / 1000.0));

    constexpr int kSealDetail = 20;
    double sealTotal = 0.0;
    for (int i = 0; i < kSealDetail; ++i) {
        const std::uint64_t offset =
            static_cast<std::uint64_t>(i * 7) % device->geometry().regionCount() * k64K;
        device->write(offset, k64K, chunk.data());
        const double moment = nowMs();
        device->seal();
        sealTotal += nowMs() - moment;
    }
    std::printf("| Seal with 1 dirty region | %.4f ms |\n", sealTotal / kSealDetail);

    std::printf("\nJournal mark cost is dominated by one fsync per write. That is the price "
                "of `durableBeforeAck = true` for the simulator and is the number a kernel "
                "backend must beat with in-kernel tracking.\n");

    device->close();
    std::string cleanup = "rm -rf " + root;
    const int ignored = std::system(cleanup.c_str());
    (void)ignored;
    return 0;
}
