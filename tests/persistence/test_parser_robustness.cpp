// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Every metadata reader is fed garbage, truncated data and bit-flipped valid
// files. The requirement is not "reject everything" (a random buffer may
// coincidentally validate) but: no crash, no out-of-bounds read, and a defined
// status. Run under ASan/UBSan this turns each parser into a checked surface.

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

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

Geometry makeGeometry()
{
    Geometry geometry;
    EXPECT_EQ(Geometry::make(8u * k64K, k64K, geometry), Status::Ok);
    return geometry;
}

class CountingHandler final : public WalHandler {
public:
    Status onRecord(const WalRecord&) override { return Status::Ok; }
};

TEST(ParserRobustness, DeltaReaderSurvivesArbitraryBytes)
{
    TempDir directory;
    const MetadataIdentity identity = MetadataIdentity::fromSeed(3);
    const Geometry geometry = makeGeometry();
    const std::string path = directory.path() + "/gen.delta";

    std::mt19937 rng(0xF02201u);

    for (std::size_t size = 0; size <= 256; size += 3) {
        std::vector<std::uint8_t> bytes(size);
        for (std::size_t i = 0; i < size; ++i) {
            bytes[i] = static_cast<std::uint8_t>(rng() & 0xFFu);
        }
        ASSERT_EQ(internal::writeFileAtomic(path, bytes), Status::Ok);

        DirtyBitmap out(geometry);
        Generation generation = 0;
        const Status status = deltaRead(path, identity, geometry, generation, out);
        EXPECT_NE(status, Status::Ok) << "garbage of size " << size << " was accepted";
    }
}

TEST(ParserRobustness, DeltaReaderRejectsEverySingleBitFlip)
{
    TempDir directory;
    const MetadataIdentity identity = MetadataIdentity::fromSeed(4);
    const Geometry geometry = makeGeometry();
    const std::string path = directory.path() + "/gen.delta";

    DirtyBitmap original(geometry);
    ASSERT_TRUE(original.markRegion(3));
    ASSERT_EQ(deltaWrite(path, identity, 1, original), Status::Ok);

    std::vector<std::uint8_t> bytes;
    ASSERT_EQ(internal::readWholeFile(path, bytes), Status::Ok);

    for (std::size_t i = 0; i < bytes.size(); ++i) {
        std::vector<std::uint8_t> corrupted = bytes;
        corrupted[i] ^= 0x01u;
        ASSERT_EQ(internal::writeFileAtomic(path, corrupted), Status::Ok);

        DirtyBitmap out(geometry);
        Generation generation = 0;
        const Status status = deltaRead(path, identity, geometry, generation, out);
        EXPECT_NE(status, Status::Ok) << "bit flip at byte " << i << " was accepted";
    }
}

TEST(ParserRobustness, SuperblockReaderSurvivesArbitraryBytes)
{
    TempDir directory;
    const MetadataIdentity identity = MetadataIdentity::fromSeed(5);
    const std::string path = directory.path() + "/manifest.bin";

    std::mt19937 rng(0xF02202u);

    for (std::size_t size = 0; size <= 2u * kSuperblockSlotSize + 32u; size += 97) {
        std::vector<std::uint8_t> bytes(size);
        for (std::size_t i = 0; i < size; ++i) {
            bytes[i] = static_cast<std::uint8_t>(rng() & 0xFFu);
        }
        ASSERT_EQ(internal::writeFileAtomic(path, bytes), Status::Ok);

        SuperblockState out;
        bool found = false;
        const Status status = superblockRead(path, identity, out, found);
        EXPECT_NE(status, Status::IoError);
    }
}

TEST(ParserRobustness, WalReplaySurvivesArbitraryBytes)
{
    TempDir directory;
    const std::string walDirectory = directory.path() + "/wal";
    ASSERT_EQ(internal::ensureDirectory(walDirectory), Status::Ok);
    const std::string segment = walDirectory + "/" + walSegmentFileName(1);

    std::mt19937 rng(0xF02203u);

    for (std::size_t size = 0; size <= 512; size += 7) {
        std::vector<std::uint8_t> bytes(size);
        for (std::size_t i = 0; i < size; ++i) {
            bytes[i] = static_cast<std::uint8_t>(rng() & 0xFFu);
        }
        ASSERT_EQ(internal::writeFileAtomic(segment, bytes), Status::Ok);

        CountingHandler handler;
        const WalReplaySummary summary = replayWal(walDirectory, 1, handler);
        EXPECT_NE(summary.status, Status::IoError);
    }
}

} // namespace
