// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Phase 5 recovery tests: the crash matrix from docs/metadata-format.md section 7,
// restricted to the rows that the current file formats can express.
//
// A crash is simulated by throwing from a named crash point. That is faithful:
// no destructor flushes anything, so any state that survives did so because the
// code wrote and fsynced it before the point where it died.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "blockchronicle/consumer.h"
#include "blockchronicle/crash_point.h"
#include "blockchronicle/persistent_device.h"
#include "blockchronicle/delta_file.h"
#include "persistence/file_io.h"
#include "support/status_printer.h"
#include "support/temp_dir.h"

namespace {

using namespace blockchronicle;

constexpr std::uint32_t k64K = 64u * 1024u;

struct SimulatedCrash {
};

class ThrowingCrash final : public CrashPoint {
public:
    void arm(const std::string& name)
    {
        target_ = name;
        armed_ = true;
    }

    void hit(const char* name) override
    {
        if (armed_ && target_ == name) {
            armed_ = false;
            throw SimulatedCrash();
        }
    }

private:
    std::string target_;
    bool armed_ = false;
};

Geometry makeGeometry(std::uint64_t regions = 64)
{
    Geometry geometry;
    EXPECT_EQ(Geometry::make(regions * k64K, k64K, geometry), Status::Ok);
    return geometry;
}

std::vector<std::uint32_t> regionsOfRanges(const std::vector<DirtyRange>& ranges)
{
    std::vector<std::uint32_t> regions;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const std::uint64_t first = ranges[i].offsetBytes / k64K;
        const std::uint64_t last =
            (ranges[i].offsetBytes + ranges[i].lengthBytes - 1u) / k64K;
        for (std::uint64_t region = first; region <= last; ++region) {
            regions.push_back(static_cast<std::uint32_t>(region));
        }
    }
    return regions;
}

class Fixture {
public:
    explicit Fixture(std::uint64_t seed = 100)
        : identity(MetadataIdentity::fromSeed(seed))
        , geometry(makeGeometry())
    {
    }

    MetadataIdentity identity;
    Geometry geometry;
    TempDir directory;
    ThrowingCrash crash;
    NoCrashPoint quiet;

    Status createDevice(std::unique_ptr<PersistentDevice>& out)
    {
        return PersistentDevice::create(directory.path(), identity, geometry,
                                        DeviceOptions{}, crash, out);
    }

    Status openDevice(std::unique_ptr<PersistentDevice>& out)
    {
        return PersistentDevice::open(directory.path(), identity, DeviceOptions{}, crash, out);
    }
};

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

TEST(Persistence, CreateAndReopenCleanly)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);
    EXPECT_EQ(device->recoveryState(), RecoveryState::Created);

    ASSERT_EQ(device->markWrite(0, 1), Status::Ok);
    ASSERT_EQ(device->markWrite(3 * k64K, 1), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);
    ASSERT_EQ(device->addConsumer("A"), Status::Ok);
    ASSERT_EQ(device->close(), Status::Ok);
    device.reset();

    ASSERT_EQ(fixture.openDevice(device), Status::Ok);
    EXPECT_EQ(device->recoveryState(), RecoveryState::Clean);
    EXPECT_EQ(device->lastSealedGeneration(), 1u);
    EXPECT_EQ(device->activeGeneration(), 2u);
    EXPECT_TRUE(device->consumers().hasConsumer("A"));
}

TEST(Persistence, HistorySurvivesRestart)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->markWrite(0, k64K), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok); // gen 1

    ASSERT_EQ(device->addConsumer("A"), Status::Ok);
    BackupSession baseline;
    ASSERT_EQ(device->beginBackup("A", baseline), Status::Ok);
    ASSERT_EQ(baseline.kind, BackupKind::Full);
    ASSERT_EQ(device->commitBackup(baseline, 1), Status::Ok);

    ASSERT_EQ(device->markWrite(5 * k64K, k64K), Status::Ok);
    ASSERT_EQ(device->markWrite(9 * k64K, k64K), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok); // gen 2
    ASSERT_EQ(device->close(), Status::Ok);
    device.reset();

    ASSERT_EQ(fixture.openDevice(device), Status::Ok);

    ConsumerStatus status;
    ASSERT_EQ(device->consumerStatus("A", status), Status::Ok);
    EXPECT_EQ(status.cursor, 1u);
    EXPECT_FALSE(status.fullBackupRequired);

    BackupSession session;
    ASSERT_EQ(device->beginBackup("A", session), Status::Ok);
    ASSERT_EQ(session.kind, BackupKind::Incremental);
    EXPECT_EQ(session.boundary, 2u);

    std::vector<DirtyRange> ranges;
    ASSERT_EQ(device->getDirtyRanges(session, 256, ranges), Status::Ok);
    EXPECT_EQ(regionsOfRanges(ranges), (std::vector<std::uint32_t>{5, 9}));
    ASSERT_EQ(device->abortBackup(session), Status::Ok);
}

TEST(Persistence, CheckpointTruncatesTheJournal)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(device->markWrite(static_cast<std::uint64_t>(i) * k64K, k64K), Status::Ok);
        ASSERT_EQ(device->seal(), Status::Ok);
    }
    ASSERT_EQ(device->checkpoint(), Status::Ok);

    std::vector<std::pair<std::uint64_t, std::string>> segments;
    ASSERT_EQ(walListSegments(fixture.directory.path() + "/wal", segments), Status::Ok);
    EXPECT_EQ(segments.size(), 1u)
        << "checkpoint must leave a single active segment, not the whole history";

    ASSERT_EQ(device->close(), Status::Ok);
    device.reset();

    ASSERT_EQ(fixture.openDevice(device), Status::Ok);
    EXPECT_EQ(device->lastSealedGeneration(), 5u);
}

TEST(Persistence, DoubleCreateIsRejected)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    std::unique_ptr<PersistentDevice> second;
    EXPECT_EQ(fixture.createDevice(second), Status::InvalidState);
}

TEST(Persistence, ForeignIdentityCannotOpen)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);
    ASSERT_EQ(device->close(), Status::Ok);
    device.reset();

    std::unique_ptr<PersistentDevice> other;
    const MetadataIdentity foreign = MetadataIdentity::fromSeed(999);
    EXPECT_EQ(PersistentDevice::open(fixture.directory.path(), foreign, DeviceOptions{},
                                     fixture.crash, other),
              Status::Corrupt);
}

// ---------------------------------------------------------------------------
// Crash matrix
// ---------------------------------------------------------------------------

TEST(Recovery, MarkedButUnsealedWritesSurviveAcrash)
{
    // Row 1-2: the mark is journaled and fsynced before it is acknowledged, so
    // an unsealed generation survives process death.
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->markWrite(2 * k64K, k64K), Status::Ok);
    ASSERT_EQ(device->markWrite(7 * k64K, k64K), Status::Ok);

    device.reset(); // crash: no close()

    ASSERT_EQ(fixture.openDevice(device), Status::Ok);
    EXPECT_EQ(device->recoveryState(), RecoveryState::Replayed);
    EXPECT_EQ(device->activeGeneration(), 1u);

    ASSERT_EQ(device->seal(), Status::Ok);
    BackupSession session;
    ASSERT_EQ(device->addConsumer("A"), Status::Ok);
    ASSERT_EQ(device->beginBackup("A", session), Status::Ok);
    EXPECT_EQ(session.kind, BackupKind::Full);
}

TEST(Recovery, CrashAfterDeltaWriteLeavesTheGenerationUnsealed)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->markWrite(0, k64K), Status::Ok);

    fixture.crash.arm("after_delta_write");
    bool crashed = false;
    try {
        device->seal();
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    EXPECT_TRUE(crashed);
    device.reset();

    ASSERT_EQ(fixture.openDevice(device), Status::Ok);
    EXPECT_EQ(device->lastSealedGeneration(), 0u)
        << "the seal was never acknowledged, so the generation stays open";
    EXPECT_EQ(device->activeGeneration(), 1u);

    // The mark itself was journaled and must still be recoverable.
    ASSERT_EQ(device->seal(), Status::Ok);
    EXPECT_EQ(device->lastSealedGeneration(), 1u);
}

TEST(Recovery, CrashAfterSealRecordMakesTheGenerationDurable)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->markWrite(4 * k64K, k64K), Status::Ok);

    fixture.crash.arm("after_seal_wal");
    bool crashed = false;
    try {
        device->seal();
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    EXPECT_TRUE(crashed);
    device.reset();

    ASSERT_EQ(fixture.openDevice(device), Status::Ok);
    EXPECT_EQ(device->lastSealedGeneration(), 1u)
        << "the SEAL record was fsynced, so replay must find it";

    ASSERT_EQ(device->addConsumer("A"), Status::Ok);
    BackupSession session;
    ASSERT_EQ(device->beginBackup("A", session), Status::Ok);
    EXPECT_EQ(session.boundary, 1u);
}

TEST(Recovery, CrashAfterConsumerCommitKeepsTheCursor)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->markWrite(0, k64K), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);

    ASSERT_EQ(device->addConsumer("A"), Status::Ok);
    BackupSession baseline;
    ASSERT_EQ(device->beginBackup("A", baseline), Status::Ok);

    fixture.crash.arm("after_consumer_commit");
    bool crashed = false;
    try {
        device->commitBackup(baseline, 1);
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    EXPECT_TRUE(crashed);
    device.reset();

    ASSERT_EQ(fixture.openDevice(device), Status::Ok);

    ConsumerStatus status;
    ASSERT_EQ(device->consumerStatus("A", status), Status::Ok);
    EXPECT_EQ(status.cursor, 1u)
        << "the commit record was durable before the reply, so it must survive";
    EXPECT_FALSE(status.fullBackupRequired);
}

TEST(Recovery, CrashBeforeSuperblockWriteKeepsTheOldReplayPoint)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->markWrite(0, k64K), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);

    fixture.crash.arm("before_superblock_write");
    bool crashed = false;
    try {
        device->checkpoint();
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    EXPECT_TRUE(crashed);
    device.reset();

    ASSERT_EQ(fixture.openDevice(device), Status::Ok);
    EXPECT_EQ(device->lastSealedGeneration(), 1u);
}

TEST(Recovery, CrashAfterSuperblockWriteKeepsTheNewReplayPoint)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->markWrite(0, k64K), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);
    ASSERT_EQ(device->markWrite(1 * k64K, k64K), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);

    fixture.crash.arm("after_superblock_write");
    bool crashed = false;
    try {
        device->checkpoint();
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    EXPECT_TRUE(crashed);
    device.reset();

    ASSERT_EQ(fixture.openDevice(device), Status::Ok);
    EXPECT_EQ(device->lastSealedGeneration(), 2u)
        << "state up to the checkpoint is in history even though the WAL was not rotated";
}

TEST(Recovery, TornJournalTailIsDiscardedWithoutLosingAcknowledgedRecords)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->markWrite(0, k64K), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);
    ASSERT_EQ(device->markWrite(1 * k64K, k64K), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);

    // Simulate the *next* append being cut off. An acknowledged record is
    // fsynced in full, so it can never be short; only a record that was in
    // flight when the process died can be torn.
    std::vector<std::pair<std::uint64_t, std::string>> segments;
    ASSERT_EQ(walListSegments(fixture.directory.path() + "/wal", segments), Status::Ok);
    ASSERT_FALSE(segments.empty());

    std::vector<std::uint8_t> bytes;
    ASSERT_EQ(internal::readWholeFile(segments.back().second, bytes), Status::Ok);

    // 12 bytes: a record magic plus a partial length field.
    const std::uint8_t partial[] = {0x57u, 0x4Cu, 0x52u, 0x31u, 0x60u, 0x00u,
                                    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    bytes.insert(bytes.end(), partial, partial + sizeof(partial));
    ASSERT_EQ(internal::writeFileAtomic(segments.back().second, bytes), Status::Ok);

    device.reset();

    ASSERT_EQ(fixture.openDevice(device), Status::Ok);
    EXPECT_EQ(device->lastSealedGeneration(), 2u)
        << "both acknowledged seals must survive; only the torn tail is dropped";
}

TEST(Recovery, MissingDeltaInvalidatesAndForcesFullBackups)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->markWrite(0, k64K), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);
    ASSERT_EQ(device->addConsumer("A"), Status::Ok);

    BackupSession baseline;
    ASSERT_EQ(device->beginBackup("A", baseline), Status::Ok);
    ASSERT_EQ(device->commitBackup(baseline, 1), Status::Ok);
    ASSERT_EQ(device->checkpoint(), Status::Ok);
    ASSERT_EQ(device->close(), Status::Ok);
    device.reset();

    ASSERT_EQ(internal::removeFile(fixture.directory.path() + "/history/"
                                   + deltaFileName(1)),
              Status::Ok);

    ASSERT_EQ(fixture.openDevice(device), Status::Ok);
    EXPECT_EQ(device->recoveryState(), RecoveryState::Invalidated);

    ConsumerStatus status;
    ASSERT_EQ(device->consumerStatus("A", status), Status::Ok);
    EXPECT_TRUE(status.fullBackupRequired)
        << "history that cannot be proven complete must force a full backup";
}

TEST(Recovery, UncleanShutdownInvalidatesWhenTheBackendCannotProveCompleteness)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;

    DeviceOptions options;
    options.durableBeforeAck = false;
    options.invalidateOnUncleanShutdown = true;
    options.journalMarks = false;

    ASSERT_EQ(PersistentDevice::create(fixture.directory.path(), fixture.identity,
                                       fixture.geometry, options, fixture.crash, device),
              Status::Ok);

    ASSERT_EQ(device->markWrite(0, k64K), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);
    ASSERT_EQ(device->addConsumer("A"), Status::Ok);

    BackupSession baseline;
    ASSERT_EQ(device->beginBackup("A", baseline), Status::Ok);
    ASSERT_EQ(device->commitBackup(baseline, 1), Status::Ok);

    device.reset(); // unclean

    ASSERT_EQ(PersistentDevice::open(fixture.directory.path(), fixture.identity, options,
                                     fixture.crash, device),
              Status::Ok);
    EXPECT_EQ(device->recoveryState(), RecoveryState::Invalidated);

    ConsumerStatus status;
    ASSERT_EQ(device->consumerStatus("A", status), Status::Ok);
    EXPECT_TRUE(status.fullBackupRequired);
}

TEST(Recovery, CleanShutdownDoesNotInvalidateAnUnprovenBackend)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;

    DeviceOptions options;
    options.durableBeforeAck = false;
    options.invalidateOnUncleanShutdown = true;
    options.journalMarks = false;

    ASSERT_EQ(PersistentDevice::create(fixture.directory.path(), fixture.identity,
                                       fixture.geometry, options, fixture.crash, device),
              Status::Ok);
    ASSERT_EQ(device->markWrite(0, k64K), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);
    ASSERT_EQ(device->close(), Status::Ok);
    device.reset();

    ASSERT_EQ(PersistentDevice::open(fixture.directory.path(), fixture.identity, options,
                                     fixture.crash, device),
              Status::Ok);
    EXPECT_NE(device->recoveryState(), RecoveryState::Invalidated);
}

TEST(Recovery, UnsafeConfigurationIsRefused)
{
    TempDir directory;
    const MetadataIdentity identity = MetadataIdentity::fromSeed(77);
    const Geometry geometry = makeGeometry();
    NoCrashPoint crash;

    DeviceOptions options;
    options.durableBeforeAck = false;
    options.invalidateOnUncleanShutdown = false;

    std::unique_ptr<PersistentDevice> device;
    EXPECT_EQ(PersistentDevice::create(directory.path(), identity, geometry, options,
                                       crash, device),
              Status::InvalidArgument);
}

} // namespace
