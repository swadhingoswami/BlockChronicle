// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Phase 4 acceptance tests: independent consumers, independent cursors, and the
// brief's three-consumer scenario as an executable specification.
//
// The load-bearing test is LateCommitRebuildsPendingWithoutDuplication: a session
// captures its pending set at begin, seals continue while the backup is running,
// and the commit must leave exactly the post-boundary deltas pending. OR cannot
// be subtracted, so an implementation that simply clears pending on commit loses
// data, and one that keeps it intact re-reports acknowledged changes.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "blockchronicle/consumer.h"
#include "blockchronicle/generation.h"
#include "blockchronicle/history.h"
#include "support/status_printer.h"

namespace {

using namespace blockchronicle;

constexpr std::uint32_t k64K = 64u * 1024u;

Geometry makeGeometry(std::uint64_t capacityBytes = 256 * k64K)
{
    Geometry geometry;
    const Status status = Geometry::make(capacityBytes, k64K, geometry);
    EXPECT_EQ(status, Status::Ok);
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

class Rig {
public:
    Rig()
        : geometry_(makeGeometry())
        , history_(geometry_)
        , consumers_(geometry_, 1)
        , manager_(history_, 1, &consumers_)
    {
    }

    // Marks the regions and seals them as one generation.
    Generation sealWith(const std::vector<std::uint32_t>& regions)
    {
        for (std::size_t i = 0; i < regions.size(); ++i) {
            EXPECT_TRUE(manager_.markRegion(regions[i]));
        }
        EXPECT_EQ(manager_.seal(), Status::Ok);
        return manager_.lastSealedGeneration();
    }

    // Registers the consumer and completes a full baseline at the current
    // boundary. Returns the baseline generation.
    Generation baseline(const std::string& consumer, BackupId backupId)
    {
        EXPECT_EQ(consumers_.addConsumer(consumer), Status::Ok);

        BackupSession session;
        EXPECT_EQ(manager_.beginBackup(consumer, session), Status::Ok);
        EXPECT_EQ(session.kind, BackupKind::Full);
        EXPECT_EQ(consumers_.commit(session, backupId), Status::Ok);
        return session.boundary;
    }

    std::vector<std::uint32_t> rangesFor(const std::string& consumer)
    {
        BackupSession session;
        EXPECT_EQ(manager_.beginBackup(consumer, session), Status::Ok);

        std::vector<DirtyRange> ranges;
        EXPECT_EQ(getDirtyRanges(session, 4096, ranges), Status::Ok);
        EXPECT_EQ(consumers_.abort(session), Status::Ok);
        return regionsOfRanges(ranges);
    }

    Geometry geometry_;
    HistoryStore history_;
    ConsumerManager consumers_;
    GenerationManager manager_;
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

TEST(ConsumerManager, RegistersAndRejectsDuplicates)
{
    ConsumerManager consumers(makeGeometry(), 1);

    EXPECT_EQ(consumers.addConsumer("backup-A"), Status::Ok);
    EXPECT_TRUE(consumers.hasConsumer("backup-A"));
    EXPECT_EQ(consumers.consumerCount(), 1u);

    EXPECT_EQ(consumers.addConsumer("backup-A"), Status::InvalidArgument);
    EXPECT_EQ(consumers.consumerCount(), 1u);
}

TEST(ConsumerManager, EnforcesMaxConsumers)
{
    ConsumerManager consumers(makeGeometry(), 1, 10);

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(consumers.addConsumer("consumer-" + std::to_string(i)), Status::Ok);
    }
    EXPECT_EQ(consumers.addConsumer("consumer-10"), Status::OutOfRange);
    EXPECT_EQ(consumers.consumerCount(), 10u);
}

TEST(ConsumerManager, RejectsMalformedIdentifiers)
{
    ConsumerManager consumers(makeGeometry(), 1);

    EXPECT_EQ(consumers.addConsumer(""), Status::InvalidArgument);
    EXPECT_EQ(consumers.addConsumer("line\nbreak"), Status::InvalidArgument);
    EXPECT_EQ(consumers.addConsumer(std::string(129, 'x')), Status::InvalidArgument);
    EXPECT_EQ(consumers.addConsumer(std::string(128, 'x')), Status::Ok);
}

TEST(ConsumerManager, RemoveRefusesWhileASessionIsOpen)
{
    Rig rig;
    rig.sealWith({1});
    EXPECT_EQ(rig.consumers_.addConsumer("A"), Status::Ok);

    BackupSession session;
    ASSERT_EQ(rig.manager_.beginBackup("A", session), Status::Ok);

    EXPECT_EQ(rig.consumers_.removeConsumer("A"), Status::InvalidState);
    ASSERT_EQ(rig.consumers_.abort(session), Status::Ok);
    EXPECT_EQ(rig.consumers_.removeConsumer("A"), Status::Ok);
    EXPECT_EQ(rig.consumers_.removeConsumer("A"), Status::NotFound);
}

// ---------------------------------------------------------------------------
// Baseline gate
// ---------------------------------------------------------------------------

TEST(ConsumerManager, NewConsumerRequiresAFullBaseline)
{
    Rig rig;
    rig.sealWith({1});

    ASSERT_EQ(rig.consumers_.addConsumer("A"), Status::Ok);

    BackupSession session;
    ASSERT_EQ(rig.manager_.beginBackup("A", session), Status::Ok);

    EXPECT_EQ(session.kind, BackupKind::Full);
    EXPECT_EQ(session.cursorAtStart, 0u);
    EXPECT_EQ(session.boundary, 1u);

    // A full session must not pretend to have an incremental range list.
    std::vector<DirtyRange> ranges;
    EXPECT_EQ(getDirtyRanges(session, 16, ranges), Status::InvalidState);
    EXPECT_TRUE(ranges.empty());

    ASSERT_EQ(rig.consumers_.commit(session, 1), Status::Ok);

    ConsumerStatus status;
    ASSERT_EQ(rig.consumers_.status("A", status), Status::Ok);
    EXPECT_FALSE(status.fullBackupRequired);
    EXPECT_EQ(status.cursor, 1u);
    EXPECT_EQ(status.lastBackupId, 1u);
    EXPECT_FALSE(status.sessionOpen);

    rig.sealWith({2});
    BackupSession incremental;
    ASSERT_EQ(rig.manager_.beginBackup("A", incremental), Status::Ok);
    EXPECT_EQ(incremental.kind, BackupKind::Incremental);
}

TEST(ConsumerManager, BeginFailsBeforeAnyGenerationIsSealed)
{
    Rig rig;
    ASSERT_EQ(rig.consumers_.addConsumer("A"), Status::Ok);

    BackupSession session;
    EXPECT_EQ(rig.manager_.beginBackup("A", session), Status::InvalidState);
}

TEST(ConsumerManager, UnknownConsumerIsNotFound)
{
    Rig rig;
    rig.sealWith({1});

    BackupSession session;
    EXPECT_EQ(rig.manager_.beginBackup("nobody", session), Status::NotFound);
}

// ---------------------------------------------------------------------------
// Incremental correctness
// ---------------------------------------------------------------------------

TEST(ConsumerManager, IncrementalRangesCoverExactlyTheGenerationsSinceTheCursor)
{
    Rig rig;
    rig.sealWith({1, 2});              // gen 1
    ASSERT_EQ(rig.baseline("A", 1), 1u);

    rig.sealWith({10, 11});            // gen 2
    rig.sealWith({20});                // gen 3

    EXPECT_EQ(rig.rangesFor("A"), (std::vector<std::uint32_t>{10, 11, 20}));
}

TEST(ConsumerManager, LateCommitRebuildsPendingWithoutDuplication)
{
    Rig rig;
    rig.sealWith({1});
    ASSERT_EQ(rig.baseline("A", 1), 1u);

    rig.sealWith({2});                 // gen 2
    rig.sealWith({3});                 // gen 3

    BackupSession session;
    ASSERT_EQ(rig.manager_.beginBackup("A", session), Status::Ok);
    ASSERT_EQ(session.boundary, 3u);

    // The backup is in flight while the device keeps writing.
    rig.sealWith({4});                 // gen 4
    rig.sealWith({2, 5});              // gen 5: region 2 changes again

    std::vector<DirtyRange> ranges;
    ASSERT_EQ(getDirtyRanges(session, 4096, ranges), Status::Ok);
    EXPECT_EQ(regionsOfRanges(ranges), (std::vector<std::uint32_t>{2, 3}))
        << "the session must report exactly its boundary, not later seals";

    ASSERT_EQ(rig.consumers_.commit(session, 2), Status::Ok);

    ConsumerStatus status;
    ASSERT_EQ(rig.consumers_.status("A", status), Status::Ok);
    EXPECT_EQ(status.cursor, 3u);
    EXPECT_EQ(status.unackedGenerations, 2u)
        << "generations 4 and 5 must remain pending after committing at 3";

    // The next backup must cover exactly generations 4 and 5. Clearing pending on
    // commit would lose {4, 5}; keeping it would re-send {2, 3}.
    rig.sealWith({6});                 // gen 6
    EXPECT_EQ(rig.rangesFor("A"), (std::vector<std::uint32_t>{2, 4, 5, 6}));
}

TEST(ConsumerManager, AbortNeverAdvancesTheCursor)
{
    Rig rig;
    rig.sealWith({1});
    ASSERT_EQ(rig.baseline("A", 1), 1u);

    rig.sealWith({7});

    BackupSession session;
    ASSERT_EQ(rig.manager_.beginBackup("A", session), Status::Ok);
    ASSERT_EQ(rig.consumers_.abort(session), Status::Ok);

    ConsumerStatus status;
    ASSERT_EQ(rig.consumers_.status("A", status), Status::Ok);
    EXPECT_EQ(status.cursor, 1u);
    EXPECT_EQ(status.lastBackupId, 1u);
    EXPECT_FALSE(status.sessionOpen);

    EXPECT_EQ(rig.rangesFor("A"), (std::vector<std::uint32_t>{7}))
        << "a failed backup must be retryable with nothing lost";
}

TEST(ConsumerManager, CommitRejectsStaleBackupIdsAndReuse)
{
    Rig rig;
    rig.sealWith({1});
    ASSERT_EQ(rig.baseline("A", 5), 1u);

    rig.sealWith({2});

    BackupSession session;
    ASSERT_EQ(rig.manager_.beginBackup("A", session), Status::Ok);
    ASSERT_EQ(rig.consumers_.commit(session, 6), Status::Ok);

    // The handle was invalidated by commit.
    EXPECT_EQ(rig.consumers_.commit(session, 7), Status::InvalidState);
    EXPECT_EQ(rig.consumers_.abort(session), Status::InvalidState);

    // A backup id that does not advance is rejected.
    BackupSession second;
    ASSERT_EQ(rig.manager_.beginBackup("A", session), Status::Ok);
    second = session;
    EXPECT_EQ(rig.consumers_.commit(second, 6), Status::InvalidState);
    EXPECT_EQ(rig.consumers_.commit(second, 5), Status::InvalidState);
    EXPECT_EQ(rig.consumers_.commit(second, 7), Status::Ok);
}

// ---------------------------------------------------------------------------
// The brief's three-consumer scenario
// ---------------------------------------------------------------------------

TEST(ConsumerManager, IndependentHistoriesSurviveAnotherConsumersSuccess)
{
    Rig rig;

    // Generations 1..10 exist before the first consumer arrives.
    for (std::uint32_t generation = 1; generation <= 10; ++generation) {
        rig.sealWith({});
    }
    ASSERT_EQ(rig.baseline("A", 1), 10u);

    for (std::uint32_t generation = 11; generation <= 15; ++generation) {
        rig.sealWith({generation});
    }
    ASSERT_EQ(rig.baseline("B", 1), 15u);

    for (std::uint32_t generation = 16; generation <= 18; ++generation) {
        rig.sealWith({generation});
    }
    ASSERT_EQ(rig.baseline("C", 1), 18u);

    rig.sealWith({19});
    rig.sealWith({20});

    // All three start a backup at the same boundary.
    BackupSession sessionA;
    BackupSession sessionB;
    BackupSession sessionC;
    ASSERT_EQ(rig.manager_.beginBackup("A", sessionA), Status::Ok);
    ASSERT_EQ(rig.manager_.beginBackup("B", sessionB), Status::Ok);
    ASSERT_EQ(rig.manager_.beginBackup("C", sessionC), Status::Ok);

    EXPECT_EQ(sessionA.boundary, 20u);
    EXPECT_EQ(sessionB.boundary, 20u);
    EXPECT_EQ(sessionC.boundary, 20u);

    std::vector<DirtyRange> rangesA;
    std::vector<DirtyRange> rangesB;
    std::vector<DirtyRange> rangesC;
    ASSERT_EQ(getDirtyRanges(sessionA, 4096, rangesA), Status::Ok);
    ASSERT_EQ(getDirtyRanges(sessionB, 4096, rangesB), Status::Ok);
    ASSERT_EQ(getDirtyRanges(sessionC, 4096, rangesC), Status::Ok);

    EXPECT_EQ(regionsOfRanges(rangesA),
              (std::vector<std::uint32_t>{11, 12, 13, 14, 15, 16, 17, 18, 19, 20}));
    EXPECT_EQ(regionsOfRanges(rangesB), (std::vector<std::uint32_t>{16, 17, 18, 19, 20}));
    EXPECT_EQ(regionsOfRanges(rangesC), (std::vector<std::uint32_t>{19, 20}));

    // B and C succeed. A fails.
    ASSERT_EQ(rig.consumers_.commit(sessionB, 2), Status::Ok);
    ASSERT_EQ(rig.consumers_.commit(sessionC, 2), Status::Ok);
    ASSERT_EQ(rig.consumers_.abort(sessionA), Status::Ok);

    ConsumerStatus statusA;
    ConsumerStatus statusB;
    ASSERT_EQ(rig.consumers_.status("A", statusA), Status::Ok);
    ASSERT_EQ(rig.consumers_.status("B", statusB), Status::Ok);
    EXPECT_EQ(statusA.cursor, 10u) << "A's failure must not advance A's cursor";
    EXPECT_EQ(statusB.cursor, 20u);

    // A's required history is intact even though B and C moved on.
    rig.sealWith({21});
    EXPECT_EQ(rig.rangesFor("A"),
              (std::vector<std::uint32_t>{11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21}));
    EXPECT_EQ(rig.rangesFor("B"), (std::vector<std::uint32_t>{21}));
    EXPECT_EQ(rig.rangesFor("C"), (std::vector<std::uint32_t>{21}));
}

TEST(ConsumerManager, TenConsumersKeepIndependentCursors)
{
    Rig rig;
    rig.sealWith({});

    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(rig.baseline("c" + std::to_string(i), 1), 1u);
    }

    // Advance a different number of generations for each consumer.
    for (std::uint32_t generation = 2; generation <= 11; ++generation) {
        rig.sealWith({generation});
        for (int i = 0; i < 10; ++i) {
            if (generation == static_cast<std::uint32_t>(i) + 2u) {
                const std::string id = "c" + std::to_string(i);
                BackupSession session;
                ASSERT_EQ(rig.manager_.beginBackup(id, session), Status::Ok);
                ASSERT_EQ(rig.consumers_.commit(session, 2), Status::Ok);
            }
        }
    }

    // Every consumer must see exactly what changed after its own cursor.
    for (int i = 0; i < 10; ++i) {
        const std::uint32_t cursor = static_cast<std::uint32_t>(i) + 2u;
        std::vector<std::uint32_t> expected;
        for (std::uint32_t generation = cursor + 1u; generation <= 11u; ++generation) {
            expected.push_back(generation);
        }
        EXPECT_EQ(rig.rangesFor("c" + std::to_string(i)), expected)
            << "consumer c" << i;
    }
}

// ---------------------------------------------------------------------------
// Invalidation
// ---------------------------------------------------------------------------

TEST(ConsumerManager, InvalidationForcesFullBackupsAndKillsOpenSessions)
{
    Rig rig;
    rig.sealWith({1});
    ASSERT_EQ(rig.baseline("A", 1), 1u);

    rig.sealWith({2});
    BackupSession open;
    ASSERT_EQ(rig.manager_.beginBackup("A", open), Status::Ok);

    const Generation epochBefore = rig.consumers_.deviceEpoch();
    rig.consumers_.invalidateAll();
    EXPECT_EQ(rig.consumers_.deviceEpoch(), epochBefore + 1u);

    ConsumerStatus status;
    ASSERT_EQ(rig.consumers_.status("A", status), Status::Ok);
    EXPECT_TRUE(status.fullBackupRequired);
    EXPECT_FALSE(status.sessionOpen);
    EXPECT_EQ(status.unackedGenerations, 0u);

    // The pre-invalidation session is dead.
    EXPECT_EQ(rig.consumers_.commit(open, 2), Status::InvalidState);

    rig.sealWith({3});
    BackupSession fresh;
    ASSERT_EQ(rig.manager_.beginBackup("A", fresh), Status::Ok);
    EXPECT_EQ(fresh.kind, BackupKind::Full);
}

// ---------------------------------------------------------------------------
// Range limits
// ---------------------------------------------------------------------------

TEST(ConsumerManager, RangeLimitIsAllOrNothing)
{
    Rig rig;
    rig.sealWith({});
    ASSERT_EQ(rig.baseline("A", 1), 1u);

    rig.sealWith({1});
    rig.sealWith({3});
    rig.sealWith({5});

    BackupSession session;
    ASSERT_EQ(rig.manager_.beginBackup("A", session), Status::Ok);

    std::vector<DirtyRange> ranges(1, DirtyRange{7, 7});
    EXPECT_EQ(getDirtyRanges(session, 2, ranges), Status::OutOfRange);
    EXPECT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].offsetBytes, 7u);

    EXPECT_EQ(rig.consumers_.abort(session), Status::Ok);
}

} // namespace
