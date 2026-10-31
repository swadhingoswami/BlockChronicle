// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/delta_file.h"

#include <cstdio>
#include <vector>

#include "blockchronicle/codec.h"
#include "blockchronicle/crc32c.h"
#include "file_io.h"
#include "object_header.h"

namespace blockchronicle {
namespace {

constexpr std::uint32_t kDeltaMagic = 0x444C4231u; // "1BLD"
constexpr std::uint16_t kDeltaVersion = 1;
constexpr std::uint16_t kDeltaKindDense = 1;

} // namespace

std::string deltaFileName(Generation generation)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "gen-%020llu.delta",
                  static_cast<unsigned long long>(generation));
    return std::string(buffer);
}

Status deltaWrite(const std::string& path,
                  const MetadataIdentity& identity,
                  Generation generation,
                  const DirtyBitmap& delta)
{
    const std::vector<Word>& words = delta.words();

    ByteWriter payload;
    for (std::size_t i = 0; i < words.size(); ++i) {
        payload.u64(words[i]);
    }

    internal::ObjectHeader header;
    header.magic = kDeltaMagic;
    header.version = kDeltaVersion;
    header.kind = kDeltaKindDense;
    header.identity = identity;
    header.generation = generation;
    header.granularity = delta.granularityBytes();
    header.regionCount = delta.regionCount();
    header.capacityBytes = delta.geometry().capacityBytes();
    header.payloadLength = static_cast<std::uint32_t>(payload.size());
    header.payloadCrc = crc32c(payload.data());

    std::vector<std::uint8_t> contents = internal::encodeObjectHeader(header);
    contents.insert(contents.end(), payload.data().begin(), payload.data().end());

    return internal::writeFileAtomic(path, contents);
}

Status deltaRead(const std::string& path,
                 const MetadataIdentity& identity,
                 const Geometry& geometry,
                 Generation& generation,
                 DirtyBitmap& out)
{
    std::vector<std::uint8_t> contents;
    const Status readStatus = internal::readWholeFile(path, contents);
    if (readStatus != Status::Ok) {
        return readStatus;
    }
    if (contents.size() < internal::kObjectHeaderSize) {
        return Status::Corrupt;
    }

    internal::ObjectHeader header;
    if (!internal::decodeObjectHeader(contents.data(), contents.size(), header)) {
        return Status::Corrupt;
    }
    if (header.magic != kDeltaMagic || header.version != kDeltaVersion
        || header.kind != kDeltaKindDense) {
        return Status::Corrupt;
    }
    if (header.identity.deviceUuid != identity.deviceUuid
        || header.identity.metadataUuid != identity.metadataUuid) {
        return Status::Corrupt;
    }
    if (header.granularity != geometry.granularityBytes()
        || header.regionCount != geometry.regionCount()
        || header.capacityBytes != geometry.capacityBytes()) {
        return Status::Corrupt;
    }
    if (header.payloadLength != contents.size() - internal::kObjectHeaderSize) {
        return Status::Corrupt;
    }

    const std::uint8_t* payload = contents.data() + internal::kObjectHeaderSize;
    if (crc32c(payload, header.payloadLength) != header.payloadCrc) {
        return Status::Corrupt;
    }

    DirtyBitmap bitmap(geometry);
    const std::size_t wordCount = bitmap.wordCount();
    if (header.payloadLength != wordCount * sizeof(Word)) {
        return Status::Corrupt;
    }

    ByteReader reader(payload, header.payloadLength);
    std::vector<Word> words(wordCount, 0u);
    for (std::size_t i = 0; i < wordCount; ++i) {
        if (!reader.u64(words[i])) {
            return Status::Corrupt;
        }
    }

    const Status setStatus = bitmap.setWords(std::move(words));
    if (setStatus != Status::Ok) {
        return Status::Corrupt;
    }

    generation = header.generation;
    out = std::move(bitmap);
    return Status::Ok;
}

} // namespace blockchronicle
