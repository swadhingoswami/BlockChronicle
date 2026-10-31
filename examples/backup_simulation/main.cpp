// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// A complete, self-contained demonstration of what BlockChronicle does:
//
//   1. create a virtual disk
//   2. write some data
//   3. take a full backup
//   4. change two regions
//   5. take an incremental backup
//   6. restore the backup and compare it byte for byte with the disk
//
// Everything happens under a temporary directory that is removed at the end.
// No root, no device mapper, no kernel module.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include "blockchronicle/simulator.h"

namespace {

using namespace blockchronicle;

constexpr std::uint32_t k64K = 64u * 1024u;
constexpr std::uint32_t kRegions = 16;
constexpr std::uint64_t kCapacity = static_cast<std::uint64_t>(kRegions) * k64K;

void step(int number, int total, const char* text)
{
    std::printf("\n[%d/%d] %s\n", number, total, text);
}

void detail(const char* text)
{
    std::printf("       %s\n", text);
}

std::vector<std::uint8_t> pattern(std::uint8_t value)
{
    return std::vector<std::uint8_t>(k64K, value);
}

} // namespace

int main()
{
    char temporary[128];
    std::snprintf(temporary, sizeof(temporary), "/tmp/blockchronicle-demo-%d",
                  static_cast<int>(::getpid()));
    const std::string root = temporary;
    const std::string metadata = root + "/meta";
    const std::string dataPath = root + "/disk.img";

    std::error_code error;
    std::filesystem::remove_all(root, error);

    const MetadataIdentity identity = MetadataIdentity::fromSeed(7);
    NoCrashPoint crash;

    std::unique_ptr<SimulatorDevice> device;
    if (SimulatorDevice::create(metadata, dataPath, identity, kCapacity, DeviceOptions{},
                                crash, device)
        != Status::Ok) {
        std::fprintf(stderr, "could not create the demo device\n");
        return 1;
    }

    std::printf("BlockChronicle demo — incremental backup of a %u KiB virtual disk\n",
                static_cast<unsigned int>(kCapacity / 1024u));

    step(1, 7, "Create a virtual disk with 16 trackable regions (64 KiB each)");
    detail(("data file:     " + dataPath).c_str());
    detail(("metadata dir:  " + metadata).c_str());

    step(2, 7, "Write 192 KiB: three regions filled with a known pattern");
    for (std::uint32_t region = 0; region < 3; ++region) {
        std::vector<std::uint8_t> bytes = pattern(static_cast<std::uint8_t>(0xA0u + region));
        const std::uint64_t offset = static_cast<std::uint64_t>(region) * k64K;
        if (device->write(offset, k64K, bytes.data()) != Status::Ok) {
            std::fprintf(stderr, "write failed\n");
            return 1;
        }
    }
    detail("regions 0, 1, 2 now hold data");

    if (device->addConsumer("backup-A") != Status::Ok) {
        std::fprintf(stderr, "could not add the consumer\n");
        return 1;
    }

    std::vector<std::uint8_t> restored(kCapacity, 0u);

    step(3, 7, "Full backup for consumer backup-A");
    {
        BackupSession session;
        SnapshotHandle snapshot;
        if (device->beginBackup("backup-A", session, snapshot) != Status::Ok) {
            return 1;
        }
        detail("kind:      full (a new consumer must copy everything first)");
        std::printf("       boundary:  generation %llu\n",
                    static_cast<unsigned long long>(session.boundary));

        if (device->readSnapshot(snapshot, 0, kCapacity, restored.data()) != Status::Ok) {
            return 1;
        }
        device->releaseSnapshot(snapshot);
        if (device->commitBackup(session, 1) != Status::Ok) {
            return 1;
        }
        detail("the consumer now holds the whole disk");
    }

    step(4, 7, "Change two regions (this is what a normal day looks like)");
    const std::uint32_t changed[2] = {5u, 11u};
    for (std::size_t i = 0; i < 2; ++i) {
        std::vector<std::uint8_t> bytes = pattern(static_cast<std::uint8_t>(0xC0u + i));
        const std::uint64_t offset = static_cast<std::uint64_t>(changed[i]) * k64K;
        if (device->write(offset, k64K, bytes.data()) != Status::Ok) {
            return 1;
        }
        std::printf("       region %2u changed\n", changed[i]);
    }

    step(5, 7, "Incremental backup for backup-A");
    std::uint64_t transferred = 0;
    {
        BackupSession session;
        SnapshotHandle snapshot;
        if (device->beginBackup("backup-A", session, snapshot) != Status::Ok) {
            return 1;
        }
        detail("kind: incremental");

        std::vector<DirtyRange> ranges;
        if (device->getDirtyRanges(session, 1024, ranges) != Status::Ok) {
            return 1;
        }
        std::printf("       dirty ranges: %zu\n", ranges.size());
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            std::printf("         offset %8llu  length %6llu\n",
                        static_cast<unsigned long long>(ranges[i].offsetBytes),
                        static_cast<unsigned long long>(ranges[i].lengthBytes));
            transferred += ranges[i].lengthBytes;
            if (device->readSnapshot(snapshot, ranges[i].offsetBytes, ranges[i].lengthBytes,
                                     restored.data() + ranges[i].offsetBytes)
                != Status::Ok) {
                return 1;
            }
        }

        device->releaseSnapshot(snapshot);
        if (device->commitBackup(session, 2) != Status::Ok) {
            return 1;
        }
    }

    step(6, 7, "Restore check: compare the consumer's copy with the real disk");
    std::vector<std::uint8_t> actual(kCapacity, 0u);
    if (device->read(0, kCapacity, actual.data()) != Status::Ok) {
        return 1;
    }

    if (restored == actual) {
        detail("MATCH — every byte of the backup equals the disk");
    } else {
        detail("MISMATCH — this would be a bug");
        for (std::size_t i = 0; i < kCapacity; ++i) {
            if (restored[i] != actual[i]) {
                std::printf("       first difference at byte %zu\n", i);
                break;
            }
        }
    }

    step(7, 7, "What this saved");
    std::printf("       full backup size:        %8llu bytes\n",
                static_cast<unsigned long long>(kCapacity));
    std::printf("       incremental transfer:    %8llu bytes\n",
                static_cast<unsigned long long>(transferred));
    std::printf("       reduction:              %8.0f%%\n",
                100.0 - (100.0 * static_cast<double>(transferred)
                         / static_cast<double>(kCapacity)));

    device->close();
    std::filesystem::remove_all(root, error);

    std::printf("\nThis is the whole idea: back up what changed, not what did not.\n");
    return restored == actual ? 0 : 2;
}
