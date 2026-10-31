// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Phase 5 tests for the on-disk primitives: checksum, explicit encoding,
// superblock slot selection, delta files, and the write-ahead log.
//
// These are the layers where a corrupt file must produce a refusal, never a
// plausible-looking wrong answer.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "blockchronicle/codec.h"
#include "blockchronicle/crc32c.h"
#include "blockchronicle/delta_file.h"
#include "blockchronicle/identity.h"
#include "blockchronicle/superblock.h"
#include "blockchronicle/wal.h"
#include "persistence/file_io.h"
#include "support/status_printer.h"
#include "support/temp_dir.h"

namespace {

using namespace blockchronicle;

constexpr std::uint32_t k64K = 64u * 1024u;

Geometry makeGeometry(std::uint64_t regions = 32)
{
    Geometry geometry;
    EXPECT_EQ(Geometry::make(regions * k64K, k64K, geometry), Status::Ok);
    return geometry;
}

class CountingHandler : public WalHandler {
public:
    Status onRecord(const WalRecord& record) override
    {
        records.push_back(record);
        return Status::Ok;
    }

    std::vector<WalRecord> records;
};

// ---------------------------------------------------------------------------
// CRC32C
// ---------------------------------------------------------------------------

TEST(Crc32c, MatchesPublishedVectors)
{
    const char digits[] = "123456789";
    EXPECT_EQ(crc32c(digits, 9), 0xE3069283u);
    EXPECT_EQ(crc32c("", 0), 0x00000000u);
    EXPECT_EQ(crc32c("a", 1), 0xC1D04330u);
}

TEST(Crc32c, IncrementalChunkingEqualsWholeBuffer)
{
    const std::string text = "the quick brown fox jumps over the lazy dog";
    const std::uint32_t whole = crc32c(text.data(), text.size());

    std::uint32_t partial = 0;
    for (std::size_t i = 0; i < text.size(); i += 7u) {
        const std::size_t chunk = std::min<std::size_t>(7u, text.size() - i);
        partial = crc32c(text.data() + i, chunk, partial);
    }
    EXPECT_EQ(partial, whole);
}

TEST(Crc32c, DetectsSingleBitFlip)
{
    std::vector<std::uint8_t> bytes(256, 0x5Au);
    const std::uint32_t before = crc32c(bytes);
    bytes[128] ^= 0x01u;
    EXPECT_NE(crc32c(bytes), before);
}

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

TEST(Codec, RoundTripsEveryWidth)
{
    ByteWriter writer;
    writer.u8(0x12u);
    writer.u16(0x3456u);
    writer.u32(0x789ABCDEu);
    writer.u64(0x0123456789ABCDEFull);
    writer.string16("consumer-A");

    ByteReader reader(writer.data().data(), writer.size());
    std::uint8_t u8 = 0;
    std::uint16_t u16 = 0;
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;
    std::string text;

    ASSERT_TRUE(reader.u8(u8));
    ASSERT_TRUE(reader.u16(u16));
    ASSERT_TRUE(reader.u32(u32));
    ASSERT_TRUE(reader.u64(u64));
    ASSERT_TRUE(reader.string16(text));

    EXPECT_EQ(u8, 0x12u);
    EXPECT_EQ(u16, 0x3456u);
    EXPECT_EQ(u32, 0x789ABCDEu);
    EXPECT_EQ(u64, 0x0123456789ABCDEFull);
    EXPECT_EQ(text, "consumer-A");
    EXPECT_TRUE(reader.good());
    EXPECT_EQ(reader.remaining(), 0u);
}

TEST(Codec, EncodesLittleEndianExplicitly)
{
    ByteWriter writer;
    writer.u32(0x01020304u);
    ASSERT_EQ(writer.size(), 4u);
    EXPECT_EQ(writer.data()[0], 0x04u);
    EXPECT_EQ(writer.data()[1], 0x03u);
    EXPECT_EQ(writer.data()[2], 0x02u);
    EXPECT_EQ(writer.data()[3], 0x01u);
}

TEST(Codec, ShortReadPoisonsTheReader)
{
    ByteWriter writer;
    writer.u16(0xBEEFu);
    ByteReader reader(writer.data().data(), writer.size());

    std::uint32_t value = 0;
    EXPECT_FALSE(reader.u32(value));
    EXPECT_FALSE(reader.good());

    std::uint16_t later = 0;
    EXPECT_FALSE(reader.u16(later)) << "a poisoned reader must keep failing";
}

TEST(Codec, RejectsTruncatedString)
{
    ByteWriter writer;
    writer.u16(10u);
    writer.u8(0x41u);

    ByteReader reader(writer.data().data(), writer.size());
    std::string text;
    EXPECT_FALSE(reader.string16(text));
    EXPECT_FALSE(reader.good());
}

// ---------------------------------------------------------------------------
// Superblock
// ---------------------------------------------------------------------------

TEST(Superblock, RoundTripsState)
{
    TempDir directory;
    const std::string path = directory.path() + "/manifest.bin";
    const MetadataIdentity identity = MetadataIdentity::fromSeed(7);

    SuperblockState written;
    written.capacityBytes = 32 * k64K;
    written.granularityBytes = k64K;
    written.regionCount = 32;
    written.activeGeneration = 5;
    written.lastSealedGeneration = 4;
    written.oldestRetainedGeneration = 2;
    written.walFirstSequence = 12;
    written.trackingValid = true;
    written.cleanShutdown = false;
    written.durableBeforeAck = true;

    PersistedConsumer consumer;
    consumer.consumerId = "backup-A";
    consumer.lastBackupId = 9;
    consumer.cursor = 3;
    consumer.cursorEpoch = 1;
    consumer.fullBackupRequired = false;
    written.consumers.push_back(consumer);

    ASSERT_EQ(superblockWrite(path, identity, written), Status::Ok);
    EXPECT_EQ(written.sequence, 1u);

    SuperblockState read;
    bool found = false;
    ASSERT_EQ(superblockRead(path, identity, read, found), Status::Ok);
    ASSERT_TRUE(found);

    EXPECT_EQ(read.sequence, 1u);
    EXPECT_EQ(read.activeGeneration, 5u);
    EXPECT_EQ(read.lastSealedGeneration, 4u);
    EXPECT_EQ(read.oldestRetainedGeneration, 2u);
    EXPECT_EQ(read.walFirstSequence, 12u);
    EXPECT_EQ(read.capacityBytes, 32 * k64K);
    EXPECT_EQ(read.granularityBytes, k64K);
    ASSERT_EQ(read.consumers.size(), 1u);
    EXPECT_EQ(read.consumers[0].consumerId, "backup-A");
    EXPECT_EQ(read.consumers[0].cursor, 3u);
    EXPECT_FALSE(read.consumers[0].fullBackupRequired);
}

TEST(Superblock, HighestSequenceWins)
{
    TempDir directory;
    const std::string path = directory.path() + "/manifest.bin";
    const MetadataIdentity identity = MetadataIdentity::fromSeed(9);

    SuperblockState state;
    state.capacityBytes = 32 * k64K;
    state.granularityBytes = k64K;
    state.regionCount = 32;

    state.activeGeneration = 2;
    ASSERT_EQ(superblockWrite(path, identity, state), Status::Ok);

    state.activeGeneration = 3;
    ASSERT_EQ(superblockWrite(path, identity, state), Status::Ok);

    state.activeGeneration = 4;
    ASSERT_EQ(superblockWrite(path, identity, state), Status::Ok);

    SuperblockState read;
    bool found = false;
    ASSERT_EQ(superblockRead(path, identity, read, found), Status::Ok);
    ASSERT_TRUE(found);
    EXPECT_EQ(read.sequence, 3u);
    EXPECT_EQ(read.activeGeneration, 4u);
}

TEST(Superblock, SurvivesOneCorruptSlot)
{
    TempDir directory;
    const std::string path = directory.path() + "/manifest.bin";
    const MetadataIdentity identity = MetadataIdentity::fromSeed(11);

    SuperblockState state;
    state.capacityBytes = 32 * k64K;
    state.granularityBytes = k64K;
    state.regionCount = 32;

    state.activeGeneration = 7;
    ASSERT_EQ(superblockWrite(path, identity, state), Status::Ok);
    state.activeGeneration = 8;
    ASSERT_EQ(superblockWrite(path, identity, state), Status::Ok);

    // Slot selection alternates: sequence 1 landed in slot 1, sequence 2 in
    // slot 0. Corrupt the stale slot and confirm the newest state survives.
    std::vector<std::uint8_t> bytes;
    ASSERT_EQ(internal::readWholeFile(path, bytes), Status::Ok);
    bytes[kSuperblockSlotSize + 100u] ^= 0xFFu;
    ASSERT_EQ(internal::writeFileAtomic(path, bytes), Status::Ok);

    SuperblockState read;
    bool found = false;
    ASSERT_EQ(superblockRead(path, identity, read, found), Status::Ok);
    ASSERT_TRUE(found);
    EXPECT_EQ(read.activeGeneration, 8u);
}

TEST(Superblock, BothSlotsCorruptIsACorruptResult)
{
    TempDir directory;
    const std::string path = directory.path() + "/manifest.bin";
    const MetadataIdentity identity = MetadataIdentity::fromSeed(12);

    SuperblockState state;
    state.capacityBytes = 32 * k64K;
    state.granularityBytes = k64K;
    state.regionCount = 32;
    ASSERT_EQ(superblockWrite(path, identity, state), Status::Ok);

    std::vector<std::uint8_t> bytes;
    ASSERT_EQ(internal::readWholeFile(path, bytes), Status::Ok);
    for (std::size_t i = 0; i < bytes.size(); i += 64u) {
        bytes[i] ^= 0xA5u;
    }
    ASSERT_EQ(internal::writeFileAtomic(path, bytes), Status::Ok);

    SuperblockState read;
    bool found = false;
    EXPECT_EQ(superblockRead(path, identity, read, found), Status::Corrupt);
    EXPECT_FALSE(found);
}

TEST(Superblock, RejectsForeignIdentity)
{
    TempDir directory;
    const std::string path = directory.path() + "/manifest.bin";
    const MetadataIdentity identity = MetadataIdentity::fromSeed(13);

    SuperblockState state;
    state.capacityBytes = 32 * k64K;
    state.granularityBytes = k64K;
    state.regionCount = 32;
    ASSERT_EQ(superblockWrite(path, identity, state), Status::Ok);

    SuperblockState read;
    bool found = false;
    EXPECT_EQ(superblockRead(path, MetadataIdentity::fromSeed(14), read, found),
              Status::Corrupt);
}

TEST(Superblock, MissingFileIsReportedAsNotFound)
{
    TempDir directory;
    SuperblockState read;
    bool found = true;
    EXPECT_EQ(superblockRead(directory.path() + "/nothing.bin",
                             MetadataIdentity::fromSeed(1), read, found),
              Status::Ok);
    EXPECT_FALSE(found);
}

// ---------------------------------------------------------------------------
// Delta files
// ---------------------------------------------------------------------------

TEST(DeltaFile, RoundTripsABitmap)
{
    TempDir directory;
    const MetadataIdentity identity = MetadataIdentity::fromSeed(21);
    const Geometry geometry = makeGeometry();

    DirtyBitmap original(geometry);
    ASSERT_EQ(original.markWrite(0, 5 * k64K), Status::Ok);
    ASSERT_TRUE(original.markRegion(20));

    const std::string path = directory.path() + "/" + deltaFileName(3);
    ASSERT_EQ(deltaWrite(path, identity, 3, original), Status::Ok);

    Generation generation = 0;
    DirtyBitmap restored(geometry);
    ASSERT_EQ(deltaRead(path, identity, geometry, generation, restored), Status::Ok);
    EXPECT_EQ(generation, 3u);
    EXPECT_TRUE(restored == original);
}

TEST(DeltaFile, RejectsCorruptPayload)
{
    TempDir directory;
    const MetadataIdentity identity = MetadataIdentity::fromSeed(22);
    const Geometry geometry = makeGeometry();

    DirtyBitmap bitmap(geometry);
    ASSERT_TRUE(bitmap.markRegion(1));

    const std::string path = directory.path() + "/gen.delta";
    ASSERT_EQ(deltaWrite(path, identity, 1, bitmap), Status::Ok);

    std::vector<std::uint8_t> bytes;
    ASSERT_EQ(internal::readWholeFile(path, bytes), Status::Ok);
    bytes.back() ^= 0x01u;
    ASSERT_EQ(internal::writeFileAtomic(path, bytes), Status::Ok);

    Generation generation = 0;
    DirtyBitmap restored(geometry);
    EXPECT_EQ(deltaRead(path, identity, geometry, generation, restored), Status::Corrupt);
}

TEST(DeltaFile, RejectsForeignIdentityOrGeometry)
{
    TempDir directory;
    const MetadataIdentity identity = MetadataIdentity::fromSeed(23);
    const Geometry geometry = makeGeometry();

    DirtyBitmap bitmap(geometry);
    const std::string path = directory.path() + "/gen.delta";
    ASSERT_EQ(deltaWrite(path, identity, 1, bitmap), Status::Ok);

    Generation generation = 0;
    DirtyBitmap restored(geometry);
    EXPECT_EQ(deltaRead(path, MetadataIdentity::fromSeed(24), geometry, generation, restored),
              Status::Corrupt);
    EXPECT_EQ(deltaRead(path, identity, makeGeometry(64), generation, restored),
              Status::Corrupt);
}

// ---------------------------------------------------------------------------
// WAL
// ---------------------------------------------------------------------------

TEST(Wal, AppendsAndReplaysInOrder)
{
    TempDir directory;
    const std::string walDirectory = directory.path() + "/wal";
    ASSERT_EQ(internal::ensureDirectory(walDirectory), Status::Ok);

    WalWriter writer;
    ASSERT_EQ(writer.open(walDirectory, 1), Status::Ok);

    std::vector<std::uint8_t> payload(3, 0x7Eu);
    ASSERT_EQ(writer.append(WalRecordType::Seal, 4, payload), Status::Ok);
    ASSERT_EQ(writer.append(WalRecordType::ConsumerCommit, 4, payload), Status::Ok);
    ASSERT_EQ(writer.append(WalRecordType::Checkpoint, 0, std::vector<std::uint8_t>()),
              Status::Ok);
    EXPECT_EQ(writer.nextSequence(), 4u);

    CountingHandler handler;
    const WalReplaySummary summary = replayWal(walDirectory, 1, handler);

    EXPECT_EQ(summary.status, Status::Ok);
    EXPECT_FALSE(summary.tornTail);
    ASSERT_EQ(handler.records.size(), 3u);
    EXPECT_EQ(handler.records[0].type, WalRecordType::Seal);
    EXPECT_EQ(handler.records[0].generation, 4u);
    EXPECT_EQ(handler.records[0].payload, payload);
    EXPECT_EQ(handler.records[1].type, WalRecordType::ConsumerCommit);
    EXPECT_EQ(handler.records[2].type, WalRecordType::Checkpoint);
    EXPECT_EQ(summary.lastSequence, 3u);
}

TEST(Wal, SkipsRecordsBeforeTheReplayPoint)
{
    TempDir directory;
    const std::string walDirectory = directory.path() + "/wal";
    ASSERT_EQ(internal::ensureDirectory(walDirectory), Status::Ok);

    WalWriter writer;
    ASSERT_EQ(writer.open(walDirectory, 1), Status::Ok);
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(writer.append(WalRecordType::Seal, static_cast<Generation>(i), {}),
                  Status::Ok);
    }

    CountingHandler handler;
    const WalReplaySummary summary = replayWal(walDirectory, 4, handler);

    EXPECT_EQ(summary.status, Status::Ok);
    ASSERT_EQ(handler.records.size(), 2u);
    EXPECT_EQ(handler.records[0].sequence, 4u);
    EXPECT_EQ(handler.records[1].sequence, 5u);
}

TEST(Wal, TruncatedTailIsTornNotCorrupt)
{
    TempDir directory;
    const std::string walDirectory = directory.path() + "/wal";
    ASSERT_EQ(internal::ensureDirectory(walDirectory), Status::Ok);

    WalWriter writer;
    ASSERT_EQ(writer.open(walDirectory, 1), Status::Ok);
    ASSERT_EQ(writer.append(WalRecordType::Seal, 1, {}), Status::Ok);
    ASSERT_EQ(writer.append(WalRecordType::Seal, 2, {}), Status::Ok);

    const std::string segment = walDirectory + "/" + walSegmentFileName(1);
    std::vector<std::uint8_t> bytes;
    ASSERT_EQ(internal::readWholeFile(segment, bytes), Status::Ok);
    // Cut the last record in half.
    ASSERT_EQ(internal::truncateFile(segment, bytes.size() - 5u), Status::Ok);

    CountingHandler handler;
    const WalReplaySummary summary = replayWal(walDirectory, 1, handler);

    EXPECT_EQ(summary.status, Status::Ok);
    EXPECT_TRUE(summary.tornTail);
    EXPECT_EQ(handler.records.size(), 1u);
    EXPECT_EQ(summary.lastSequence, 1u);
    EXPECT_GT(summary.tornSegmentValidLength, 0u);
}

TEST(Wal, MidLogCorruptionIsRefused)
{
    TempDir directory;
    const std::string walDirectory = directory.path() + "/wal";
    ASSERT_EQ(internal::ensureDirectory(walDirectory), Status::Ok);

    WalWriter writer;
    ASSERT_EQ(writer.open(walDirectory, 1), Status::Ok);
    ASSERT_EQ(writer.append(WalRecordType::Seal, 1, {}), Status::Ok);
    ASSERT_EQ(writer.append(WalRecordType::Seal, 2, {}), Status::Ok);
    ASSERT_EQ(writer.append(WalRecordType::Seal, 3, {}), Status::Ok);

    const std::string segment = walDirectory + "/" + walSegmentFileName(1);
    std::vector<std::uint8_t> bytes;
    ASSERT_EQ(internal::readWholeFile(segment, bytes), Status::Ok);
    // Corrupt a byte in the second record, leaving a valid third record after it.
    bytes[40] ^= 0xFFu;
    ASSERT_EQ(internal::writeFileAtomic(segment, bytes), Status::Ok);

    CountingHandler handler;
    const WalReplaySummary summary = replayWal(walDirectory, 1, handler);
    EXPECT_EQ(summary.status, Status::Corrupt);
}

TEST(Wal, RemovesSegmentsBeforeTheReplayPoint)
{
    TempDir directory;
    const std::string walDirectory = directory.path() + "/wal";
    ASSERT_EQ(internal::ensureDirectory(walDirectory), Status::Ok);

    WalWriter writer;
    ASSERT_EQ(writer.open(walDirectory, 1), Status::Ok);
    ASSERT_EQ(writer.append(WalRecordType::Seal, 1, {}), Status::Ok);
    ASSERT_EQ(writer.append(WalRecordType::Seal, 2, {}), Status::Ok);
    ASSERT_EQ(writer.rotate(), Status::Ok); // new segment starts at 3
    ASSERT_EQ(writer.append(WalRecordType::Seal, 3, {}), Status::Ok);

    std::vector<std::pair<std::uint64_t, std::string>> segments;
    ASSERT_EQ(walListSegments(walDirectory, segments), Status::Ok);
    ASSERT_EQ(segments.size(), 2u);

    ASSERT_EQ(walRemoveSegmentsBefore(walDirectory, 3), Status::Ok);
    ASSERT_EQ(walListSegments(walDirectory, segments), Status::Ok);
    ASSERT_EQ(segments.size(), 1u);
    EXPECT_EQ(segments[0].first, 3u);
}

} // namespace
