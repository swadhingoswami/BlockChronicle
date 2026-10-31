// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// End-to-end workflow test: the only test that compares the ranges the engine
// reports against the bytes a consumer would actually restore.
//
// A shadow image represents what the consumer holds. After every round it must
// equal the device contents at the backup boundary. If a range were missing, the
// shadow would keep stale bytes; if a range were spurious, the extra read is
// harmless but the test still detects the mismatch.

#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "blockchronicle/simulator.h"
#include "support/status_printer.h"
#include "support/temp_dir.h"

namespace {

using namespace blockchronicle;

constexpr std::uint32_t k64K = 64u * 1024u;

class WorkflowFixture {
public:
    WorkflowFixture()
        : capacity_(16u * k64K)
    {
        image_.assign(static_cast<std::size_t>(capacity_), 0u);
    }

    Status create()
    {
        return SimulatorDevice::create(directory_.path() + "/meta",
                                       directory_.path() + "/disk.img",
                                       MetadataIdentity::fromSeed(41), capacity_,
                                       DeviceOptions{}, crash_, device_);
    }

    std::uint64_t capacity_;
    TempDir directory_;
    NoCrashPoint crash_;
    std::unique_ptr<SimulatorDevice> device_;
    std::vector<std::uint8_t> image_; // consumer's shadow copy
};

TEST(BackupWorkflow, IncrementalRoundsReconstructTheDeviceExactly)
{
    WorkflowFixture fixture;
    ASSERT_EQ(fixture.create(), Status::Ok);

    ASSERT_EQ(fixture.device_->addConsumer("A"), Status::Ok);

    BackupId nextBackupId = 1;

    // Baseline: the consumer copies the whole device at the boundary.
    {
        BackupSession session;
        SnapshotHandle snapshot;
        ASSERT_EQ(fixture.device_->beginBackup("A", session, snapshot), Status::Ok);
        ASSERT_EQ(session.kind, BackupKind::Full);

        ASSERT_EQ(fixture.device_->readSnapshot(snapshot, 0, fixture.capacity_,
                                                fixture.image_.data()),
                  Status::Ok);

        ASSERT_EQ(fixture.device_->releaseSnapshot(snapshot), Status::Ok);
        ASSERT_EQ(fixture.device_->commitBackup(session, nextBackupId++), Status::Ok);
        EXPECT_EQ(fixture.image_, std::vector<std::uint8_t>(fixture.capacity_, 0u));
    }

    std::mt19937 rng(0x1234ABCDu);

    for (int round = 1; round <= 5; ++round) {
        // The consumer's image must end this round equal to the device contents
        // at the boundary. `expected` is that state and is built as we write.
        std::vector<std::uint8_t> expected = fixture.image_;

        std::vector<std::uint32_t> touched;
        for (int i = 0; i < 3; ++i) {
            const std::uint32_t region = rng() % 16u;
            touched.push_back(region);

            const std::uint8_t value = static_cast<std::uint8_t>(round * 16 + i);
            std::vector<std::uint8_t> bytes(k64K, value);
            const std::uint64_t offset = static_cast<std::uint64_t>(region) * k64K;

            ASSERT_EQ(fixture.device_->write(offset, k64K, bytes.data()), Status::Ok);
            std::memcpy(expected.data() + offset, bytes.data(), k64K);
        }

        BackupSession session;
        SnapshotHandle snapshot;
        ASSERT_EQ(fixture.device_->beginBackup("A", session, snapshot), Status::Ok);
        ASSERT_EQ(session.kind, BackupKind::Incremental);

        // A write that lands after the snapshot must not appear in it.
        const std::uint32_t lateRegion = (touched[0] + 7u) % 16u;
        std::vector<std::uint8_t> lateBytes(k64K, 0xEEu);
        const std::uint64_t lateOffset = static_cast<std::uint64_t>(lateRegion) * k64K;
        ASSERT_EQ(fixture.device_->write(lateOffset, k64K, lateBytes.data()), Status::Ok);

        std::vector<std::uint8_t> fromSnapshot(k64K, 0u);
        ASSERT_EQ(fixture.device_->readSnapshot(snapshot, lateOffset, k64K,
                                                fromSnapshot.data()),
                  Status::Ok);
        std::vector<std::uint8_t> expectedLate(k64K, 0u);
        std::memcpy(expectedLate.data(), expected.data() + lateOffset, k64K);
        EXPECT_EQ(fromSnapshot, expectedLate)
            << "round " << round
            << ": the snapshot must reflect the boundary, not the late write";

        // Transfer only the reported ranges into the consumer's image.
        std::vector<DirtyRange> ranges;
        ASSERT_EQ(fixture.device_->getDirtyRanges(session, 4096, ranges), Status::Ok);
        ASSERT_FALSE(ranges.empty());

        for (std::size_t i = 0; i < ranges.size(); ++i) {
            ASSERT_EQ(fixture.device_->readSnapshot(snapshot, ranges[i].offsetBytes,
                                                    ranges[i].lengthBytes,
                                                    fixture.image_.data()
                                                        + ranges[i].offsetBytes),
                      Status::Ok);
        }

        EXPECT_EQ(fixture.image_, expected)
            << "round " << round
            << ": the consumer image does not match the device at the boundary";

        for (std::size_t i = 0; i < touched.size(); ++i) {
            const std::uint64_t offset = static_cast<std::uint64_t>(touched[i]) * k64K;
            bool covered = false;
            for (std::size_t r = 0; r < ranges.size(); ++r) {
                if (offset >= ranges[r].offsetBytes
                    && offset < ranges[r].offsetBytes + ranges[r].lengthBytes) {
                    covered = true;
                    break;
                }
            }
            EXPECT_TRUE(covered) << "round " << round << ": region " << touched[i]
                                 << " changed but was not reported";
        }

        ASSERT_EQ(fixture.device_->releaseSnapshot(snapshot), Status::Ok);
        ASSERT_EQ(fixture.device_->commitBackup(session, nextBackupId++), Status::Ok);

        // The late write belongs to the consumer's next incremental round.
        std::memcpy(fixture.image_.data() + lateOffset, lateBytes.data(), k64K);
    }
}

TEST(BackupWorkflow, TwoConsumersWithDifferentCursorsReceiveDifferentRanges)
{
    WorkflowFixture fixture;
    ASSERT_EQ(fixture.create(), Status::Ok);
    ASSERT_EQ(fixture.device_->addConsumer("A"), Status::Ok);
    ASSERT_EQ(fixture.device_->addConsumer("B"), Status::Ok);

    // Both baseline at generation 1.
    {
        BackupSession a;
        SnapshotHandle sa;
        ASSERT_EQ(fixture.device_->beginBackup("A", a, sa), Status::Ok);
        ASSERT_EQ(fixture.device_->releaseSnapshot(sa), Status::Ok);
        ASSERT_EQ(fixture.device_->commitBackup(a, 1), Status::Ok);

        BackupSession b;
        SnapshotHandle sb;
        ASSERT_EQ(fixture.device_->beginBackup("B", b, sb), Status::Ok);
        ASSERT_EQ(fixture.device_->releaseSnapshot(sb), Status::Ok);
        ASSERT_EQ(fixture.device_->commitBackup(b, 1), Status::Ok);
    }

    // A advances past generation 2; B does not.
    {
        std::vector<std::uint8_t> bytes(k64K, 0xA1u);
        ASSERT_EQ(fixture.device_->write(2u * k64K, k64K, bytes.data()), Status::Ok);

        BackupSession a;
        SnapshotHandle sa;
        ASSERT_EQ(fixture.device_->beginBackup("A", a, sa), Status::Ok);
        ASSERT_EQ(fixture.device_->releaseSnapshot(sa), Status::Ok);
        ASSERT_EQ(fixture.device_->commitBackup(a, 2), Status::Ok);
    }

    {
        std::vector<std::uint8_t> bytes(k64K, 0xB2u);
        ASSERT_EQ(fixture.device_->write(5u * k64K, k64K, bytes.data()), Status::Ok);
    }

    std::vector<DirtyRange> rangesA;
    std::vector<DirtyRange> rangesB;
    {
        BackupSession a;
        SnapshotHandle sa;
        ASSERT_EQ(fixture.device_->beginBackup("A", a, sa), Status::Ok);
        ASSERT_EQ(fixture.device_->getDirtyRanges(a, 4096, rangesA), Status::Ok);
        ASSERT_EQ(fixture.device_->releaseSnapshot(sa), Status::Ok);
        ASSERT_EQ(fixture.device_->abortBackup(a), Status::Ok);

        BackupSession b;
        SnapshotHandle sb;
        ASSERT_EQ(fixture.device_->beginBackup("B", b, sb), Status::Ok);
        ASSERT_EQ(fixture.device_->getDirtyRanges(b, 4096, rangesB), Status::Ok);
        ASSERT_EQ(fixture.device_->releaseSnapshot(sb), Status::Ok);
        ASSERT_EQ(fixture.device_->abortBackup(b), Status::Ok);
    }

    ASSERT_EQ(rangesA.size(), 1u);
    EXPECT_EQ(rangesA[0].offsetBytes, 5u * k64K) << "A already has region 2";

    ASSERT_EQ(rangesB.size(), 2u) << "B still needs regions 2 and 5";
    EXPECT_EQ(rangesB[0].offsetBytes, 2u * k64K);
    EXPECT_EQ(rangesB[1].offsetBytes, 5u * k64K);
}

TEST(BackupWorkflow, DataSurvivesCloseAndReopen)
{
    WorkflowFixture fixture;
    ASSERT_EQ(fixture.create(), Status::Ok);

    std::vector<std::uint8_t> written(k64K, 0x5Au);
    ASSERT_EQ(fixture.device_->write(3u * k64K, k64K, written.data()), Status::Ok);
    ASSERT_EQ(fixture.device_->close(), Status::Ok);
    fixture.device_.reset();

    std::unique_ptr<SimulatorDevice> reopened;
    ASSERT_EQ(SimulatorDevice::open(fixture.directory_.path() + "/meta",
                                    fixture.directory_.path() + "/disk.img",
                                    MetadataIdentity::fromSeed(41), DeviceOptions{},
                                    fixture.crash_, reopened),
              Status::Ok);

    std::vector<std::uint8_t> readBack(k64K, 0u);
    ASSERT_EQ(reopened->read(3u * k64K, k64K, readBack.data()), Status::Ok);
    EXPECT_EQ(readBack, written);
}

} // namespace
