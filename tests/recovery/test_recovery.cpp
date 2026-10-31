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
#include "blockchronicle/verify.h"
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


// ---------------------------------------------------------------------------
// Retention (garbage collection)
// ---------------------------------------------------------------------------

TEST(Retention, GarbageCollectionDropsOnlyDeliveredGenerations)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(device->markWrite(static_cast<std::uint64_t>(i) * k64K, 1), Status::Ok);
        ASSERT_EQ(device->seal(), Status::Ok);
    }

    ASSERT_EQ(device->addConsumer("A"), Status::Ok);
    BackupSession baseline;
    ASSERT_EQ(device->beginBackup("A", baseline), Status::Ok);
    ASSERT_EQ(baseline.boundary, 5u);
    ASSERT_EQ(device->commitBackup(baseline, 1), Status::Ok);

    GcResult result;
    ASSERT_EQ(device->collectGarbage(0, result), Status::Ok);
    EXPECT_EQ(result.droppedGenerations, 5u);
    EXPECT_EQ(result.droppedThrough, 5u);
    EXPECT_TRUE(result.invalidated.empty()) << "A had already received generations 1..5";

    // A still gets everything after its cursor.
    ASSERT_EQ(device->markWrite(9u * k64K, 1), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);

    BackupSession session;
    ASSERT_EQ(device->beginBackup("A", session), Status::Ok);
    std::vector<DirtyRange> ranges;
    ASSERT_EQ(device->getDirtyRanges(session, 4096, ranges), Status::Ok);
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].offsetBytes, 9u * k64K);
    ASSERT_EQ(device->abortBackup(session), Status::Ok);
}

TEST(Retention, PolicyThatWouldBreakAConsumerInvalidatesItFirst)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(device->markWrite(static_cast<std::uint64_t>(i) * k64K, 1), Status::Ok);
        ASSERT_EQ(device->seal(), Status::Ok);
    }

    ASSERT_EQ(device->addConsumer("A"), Status::Ok);
    BackupSession baseline;
    ASSERT_EQ(device->beginBackup("A", baseline), Status::Ok);
    ASSERT_EQ(baseline.boundary, 10u);

    // Put A's cursor back at generation 1 by committing only what a baseline at
    // 1 would have: simulate by aborting and using the stateless path.
    ASSERT_EQ(device->abortBackup(baseline), Status::Ok);
    ASSERT_EQ(device->commitBackupAt("A", 1, 1), Status::Ok);

    // Keep only 3 generations. Generations 1..7 must go, and A cannot survive.
    GcResult result;
    ASSERT_EQ(device->collectGarbage(3, result), Status::Ok);
    EXPECT_EQ(result.droppedThrough, 7u);
    ASSERT_EQ(result.invalidated.size(), 1u);
    EXPECT_EQ(result.invalidated[0], "A");

    ConsumerStatus status;
    ASSERT_EQ(device->consumerStatus("A", status), Status::Ok);
    EXPECT_TRUE(status.fullBackupRequired)
        << "deleting history a consumer still needs must force a full backup";

    // The retained range is contiguous and starts after the drop.
    DirtyBitmap out(device->geometry());
    EXPECT_EQ(device->dirtySince(6, 10, out), Status::NotFound);
    EXPECT_EQ(device->dirtySince(7, 10, out), Status::Ok);
}

TEST(Retention, CrashAfterInvalidationKeepsHistoryUntilTheNextAttempt)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    for (int i = 0; i < 6; ++i) {
        ASSERT_EQ(device->markWrite(static_cast<std::uint64_t>(i) * k64K, 1), Status::Ok);
        ASSERT_EQ(device->seal(), Status::Ok);
    }
    ASSERT_EQ(device->addConsumer("A"), Status::Ok);
    ASSERT_EQ(device->commitBackupAt("A", 1, 1), Status::Ok);

    fixture.crash.arm("after_gc_invalidate");
    bool crashed = false;
    try {
        GcResult result;
        device->collectGarbage(2, result);
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    EXPECT_TRUE(crashed);
    device.reset();

    ASSERT_EQ(fixture.openDevice(device), Status::Ok);

    ConsumerStatus status;
    ASSERT_EQ(device->consumerStatus("A", status), Status::Ok);
    EXPECT_TRUE(status.fullBackupRequired)
        << "the invalidation was durable before any deletion";

    // The checkpoint moved the retention floor before the crash, so recovery
    // starts at generation 5 and the reclaimed files must already be gone.
    DirtyBitmap out(device->geometry());
    EXPECT_EQ(device->dirtySince(1, 6, out), Status::NotFound);
    EXPECT_EQ(device->dirtySince(4, 6, out), Status::Ok);
    EXPECT_FALSE(internal::fileExists(fixture.directory.path() + "/history/"
                                      + deltaFileName(1)));

    GcResult second;
    ASSERT_EQ(device->collectGarbage(2, second), Status::Ok);
    EXPECT_GT(second.droppedGenerations, 0u);
}

// ---------------------------------------------------------------------------
// Read-only verification
// ---------------------------------------------------------------------------

TEST(Verify, ReportsAHealthyStore)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->markWrite(0, 1), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);
    ASSERT_EQ(device->addConsumer("A"), Status::Ok);
    ASSERT_EQ(device->close(), Status::Ok);
    device.reset();

    VerifyReport report;
    ASSERT_EQ(verifyDevice(fixture.directory.path(), fixture.identity, report), Status::Ok);

    EXPECT_TRUE(report.superblockValid);
    EXPECT_TRUE(report.identityValid);
    EXPECT_TRUE(report.historyComplete);
    EXPECT_TRUE(report.journalClean);
    EXPECT_FALSE(report.uncleanShutdown);
    EXPECT_EQ(report.generationsRetained, 1u);
    EXPECT_EQ(report.consumers, 1u);
    EXPECT_TRUE(report.healthy()) << report.firstError;
}

TEST(Verify, DetectsAMissingDelta)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->markWrite(0, 1), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);
    ASSERT_EQ(device->checkpoint(), Status::Ok);
    ASSERT_EQ(device->close(), Status::Ok);
    device.reset();

    ASSERT_EQ(internal::removeFile(fixture.directory.path() + "/history/"
                                   + deltaFileName(1)),
              Status::Ok);

    VerifyReport report;
    ASSERT_EQ(verifyDevice(fixture.directory.path(), fixture.identity, report), Status::Ok);
    EXPECT_FALSE(report.historyComplete);
    EXPECT_FALSE(report.healthy());
    EXPECT_NE(report.firstError.find("generation 1"), std::string::npos);
}

TEST(Verify, ReportsUncleanShutdownWithoutFailing)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);
    ASSERT_EQ(device->markWrite(0, 1), Status::Ok);
    device.reset(); // unclean

    VerifyReport report;
    ASSERT_EQ(verifyDevice(fixture.directory.path(), fixture.identity, report), Status::Ok);
    EXPECT_TRUE(report.uncleanShutdown);
    EXPECT_FALSE(report.healthy());
}


// ---------------------------------------------------------------------------
// Single-writer guard
// ---------------------------------------------------------------------------

TEST(Persistence, SecondOpenIsRefusedWhileTheFirstIsAlive)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> first;
    ASSERT_EQ(fixture.createDevice(first), Status::Ok);

    std::unique_ptr<PersistentDevice> second;
    EXPECT_EQ(fixture.openDevice(second), Status::InvalidState)
        << "two processes writing one metadata directory would corrupt it";
    EXPECT_TRUE(second == nullptr);

    first.reset(); // releases the lock

    EXPECT_EQ(fixture.openDevice(second), Status::Ok);
}

// ---------------------------------------------------------------------------
// Analysis snapshots
// ---------------------------------------------------------------------------

TEST(Snapshots, SaveListAndDeleteLeaveOperationalHistoryIntact)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->markWrite(0, 1), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);
    ASSERT_EQ(device->markWrite(3u * k64K, 1), Status::Ok);
    ASSERT_EQ(device->seal(), Status::Ok);

    EXPECT_EQ(device->saveSnapshot(1, "diagnostic"), Status::Ok);
    EXPECT_EQ(device->saveSnapshot(2, "before-upgrade"), Status::Ok);

    std::vector<std::string> labels;
    ASSERT_EQ(device->listSnapshots(labels), Status::Ok);
    EXPECT_EQ(labels, (std::vector<std::string>{"before-upgrade", "diagnostic"}));

    EXPECT_EQ(device->saveSnapshot(1, "bad/label"), Status::InvalidArgument);
    EXPECT_EQ(device->saveSnapshot(99, "missing"), Status::NotFound);

    ASSERT_EQ(device->deleteSnapshot("diagnostic"), Status::Ok);
    EXPECT_EQ(device->deleteSnapshot("diagnostic"), Status::NotFound);

    ASSERT_EQ(device->listSnapshots(labels), Status::Ok);
    ASSERT_EQ(labels.size(), 1u);

    // The operational chain is untouched: snapshots are a separate namespace.
    DirtyBitmap out(device->geometry());
    ASSERT_EQ(device->dirtySince(0, 2, out), Status::Ok);
    EXPECT_EQ(out.countDirty(), 2u);
}

TEST(Consumers, ListAndRemove)
{
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->addConsumer("A"), Status::Ok);
    ASSERT_EQ(device->addConsumer("B"), Status::Ok);

    std::vector<ConsumerStatus> consumers;
    ASSERT_EQ(device->listConsumers(consumers), Status::Ok);
    ASSERT_EQ(consumers.size(), 2u);
    EXPECT_EQ(consumers[0].consumerId, "A");
    EXPECT_TRUE(consumers[0].fullBackupRequired);

    EXPECT_EQ(device->removeConsumer("A"), Status::Ok);
    EXPECT_EQ(device->removeConsumer("A"), Status::NotFound);

    ASSERT_EQ(device->listConsumers(consumers), Status::Ok);
    ASSERT_EQ(consumers.size(), 1u);
    EXPECT_EQ(consumers[0].consumerId, "B");
}


TEST(Persistence, CleanShutdownSealsPendingWritesBeforeTruncatingTheJournal)
{
    // Regression: checkpoint() advances walFirstSequence and rotates the log.
    // Marks in the still-active generation live only in that log, so without
    // sealing first a clean shutdown silently dropped acknowledged writes.
    Fixture fixture;
    std::unique_ptr<PersistentDevice> device;
    ASSERT_EQ(fixture.createDevice(device), Status::Ok);

    ASSERT_EQ(device->markWrite(5u * k64K, 1), Status::Ok);
    ASSERT_EQ(device->close(), Status::Ok); // checkpoints and truncates
    device.reset();

    ASSERT_EQ(fixture.openDevice(device), Status::Ok);
    EXPECT_GE(device->lastSealedGeneration(), 1u)
        << "the pending generation must be sealed before the journal is cut";

    DirtyBitmap out(device->geometry());
    ASSERT_EQ(device->dirtySince(0, device->lastSealedGeneration(), out), Status::Ok);
    EXPECT_TRUE(out.test(5))
        << "a mark acknowledged before a clean shutdown must survive it";
}

} // namespace
