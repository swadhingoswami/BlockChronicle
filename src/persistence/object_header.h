// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Common fixed header for standalone metadata objects (superblock, delta files).
// The WAL has its own record framing because records are appended in sequence.

#ifndef BLOCKCHRONICLE_SRC_PERSISTENCE_OBJECT_HEADER_H
#define BLOCKCHRONICLE_SRC_PERSISTENCE_OBJECT_HEADER_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "blockchronicle/codec.h"
#include "blockchronicle/crc32c.h"
#include "blockchronicle/identity.h"

namespace blockchronicle {
namespace internal {

constexpr std::uint32_t kObjectHeaderSize = 88u;

struct ObjectHeader {
    std::uint32_t magic = 0;
    std::uint16_t version = 1;
    std::uint16_t kind = 0;
    std::uint32_t headerLength = kObjectHeaderSize;
    std::uint32_t payloadLength = 0;
    MetadataIdentity identity;
    std::uint64_t generation = 0;
    std::uint32_t granularity = 0;
    std::uint32_t regionCount = 0;
    std::uint64_t capacityBytes = 0;
    std::uint32_t payloadCrc = 0;
};

inline std::vector<std::uint8_t> encodeObjectHeader(const ObjectHeader& header)
{
    ByteWriter writer;
    writer.u32(header.magic);
    writer.u16(header.version);
    writer.u16(header.kind);
    writer.u32(header.headerLength);
    writer.u32(header.payloadLength);
    writer.bytes(header.identity.deviceUuid.data(), 16);
    writer.bytes(header.identity.metadataUuid.data(), 16);
    writer.u64(header.identity.deviceEpoch);
    writer.u64(header.generation);
    writer.u32(header.granularity);
    writer.u32(header.regionCount);
    writer.u64(header.capacityBytes);
    writer.u32(header.payloadCrc);

    const std::uint32_t headerCrc = crc32c(writer.data().data(), writer.size());
    writer.u32(headerCrc);
    return writer.data();
}

inline bool decodeObjectHeader(const std::uint8_t* data, std::size_t size, ObjectHeader& out)
{
    if (size < kObjectHeaderSize) {
        return false;
    }

    ByteReader reader(data, kObjectHeaderSize);
    std::uint32_t storedCrc = 0;
    if (!reader.u32(out.magic) || !reader.u16(out.version) || !reader.u16(out.kind)
        || !reader.u32(out.headerLength) || !reader.u32(out.payloadLength)
        || !reader.bytes(out.identity.deviceUuid.data(), 16)
        || !reader.bytes(out.identity.metadataUuid.data(), 16)
        || !reader.u64(out.identity.deviceEpoch) || !reader.u64(out.generation)
        || !reader.u32(out.granularity) || !reader.u32(out.regionCount)
        || !reader.u64(out.capacityBytes) || !reader.u32(out.payloadCrc)
        || !reader.u32(storedCrc)) {
        return false;
    }

    return crc32c(data, kObjectHeaderSize - 4u) == storedCrc;
}

} // namespace internal
} // namespace blockchronicle

#endif // BLOCKCHRONICLE_SRC_PERSISTENCE_OBJECT_HEADER_H
