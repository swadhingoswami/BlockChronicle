// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/superblock.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "blockchronicle/codec.h"
#include "blockchronicle/crc32c.h"
#include "file_io.h"
#include "object_header.h"

namespace blockchronicle {
namespace {

constexpr std::uint32_t kSuperblockMagic = 0x53424331u; // "1CBS"
constexpr std::uint16_t kSuperblockVersion = 1;
constexpr std::uint16_t kSuperblockKind = 1;
constexpr std::uint32_t kSlotCount = 2;

std::vector<std::uint8_t> encodePayload(const SuperblockState& state)
{
    ByteWriter writer;
    writer.u64(state.sequence);
    writer.u64(state.activeGeneration);
    writer.u64(state.lastSealedGeneration);
    writer.u64(state.oldestRetainedGeneration);
    writer.u64(state.walFirstSequence);
    writer.u64(state.capacityBytes);
    writer.u32(state.granularityBytes);
    writer.u32(state.regionCount);
    writer.u8(state.trackingValid ? 1u : 0u);
    writer.u8(state.cleanShutdown ? 1u : 0u);
    writer.u8(state.durableBeforeAck ? 1u : 0u);
    writer.u8(0u);
    writer.u32(static_cast<std::uint32_t>(state.consumers.size()));

    for (std::size_t i = 0; i < state.consumers.size(); ++i) {
        const PersistedConsumer& consumer = state.consumers[i];
        writer.string16(consumer.consumerId);
        writer.u64(consumer.lastBackupId);
        writer.u64(consumer.cursor);
        writer.u64(consumer.cursorEpoch);
        writer.u8(consumer.fullBackupRequired ? 1u : 0u);
    }
    return writer.data();
}

bool decodePayload(const std::vector<std::uint8_t>& payload, SuperblockState& out)
{
    ByteReader reader(payload.data(), payload.size());

    std::uint8_t trackingValid = 0;
    std::uint8_t cleanShutdown = 0;
    std::uint8_t durableBeforeAck = 0;
    std::uint8_t reserved = 0;
    std::uint32_t consumerCount = 0;

    if (!reader.u64(out.sequence) || !reader.u64(out.activeGeneration)
        || !reader.u64(out.lastSealedGeneration) || !reader.u64(out.oldestRetainedGeneration)
        || !reader.u64(out.walFirstSequence) || !reader.u64(out.capacityBytes)
        || !reader.u32(out.granularityBytes) || !reader.u32(out.regionCount)
        || !reader.u8(trackingValid)
        || !reader.u8(cleanShutdown) || !reader.u8(durableBeforeAck)
        || !reader.u8(reserved) || !reader.u32(consumerCount)) {
        return false;
    }
    (void)reserved;

    out.trackingValid = trackingValid != 0u;
    out.cleanShutdown = cleanShutdown != 0u;
    out.durableBeforeAck = durableBeforeAck != 0u;
    out.consumers.clear();
    out.consumers.reserve(consumerCount);

    for (std::uint32_t i = 0; i < consumerCount; ++i) {
        PersistedConsumer consumer;
        std::uint8_t fullRequired = 0;
        if (!reader.string16(consumer.consumerId) || !reader.u64(consumer.lastBackupId)
            || !reader.u64(consumer.cursor) || !reader.u64(consumer.cursorEpoch)
            || !reader.u8(fullRequired)) {
            return false;
        }
        consumer.fullBackupRequired = fullRequired != 0u;
        out.consumers.push_back(consumer);
    }

    return reader.good() && reader.remaining() == 0u;
}

// Decodes one 4 KiB slot. Returns true only for a complete, identity-matching,
// checksum-valid slot.
bool decodeSlot(const std::uint8_t* slot,
                std::size_t slotSize,
                const MetadataIdentity& identity,
                internal::ObjectHeader& header,
                SuperblockState& out)
{
    if (!internal::decodeObjectHeader(slot, slotSize, header)) {
        return false;
    }
    if (header.magic != kSuperblockMagic || header.version != kSuperblockVersion
        || header.kind != kSuperblockKind) {
        return false;
    }
    if (header.identity.deviceUuid != identity.deviceUuid
        || header.identity.metadataUuid != identity.metadataUuid) {
        return false;
    }
    if (header.payloadLength > slotSize - internal::kObjectHeaderSize) {
        return false;
    }

    const std::uint8_t* payload = slot + internal::kObjectHeaderSize;
    if (crc32c(payload, header.payloadLength) != header.payloadCrc) {
        return false;
    }

    std::vector<std::uint8_t> payloadBytes(payload, payload + header.payloadLength);
    if (!decodePayload(payloadBytes, out)) {
        return false;
    }

    out.deviceEpoch = header.identity.deviceEpoch;
    return true;
}

Status writeSlot(const std::string& path,
                 std::uint32_t slotIndex,
                 const MetadataIdentity& identity,
                 SuperblockState& state)
{
    internal::ObjectHeader header;
    header.magic = kSuperblockMagic;
    header.version = kSuperblockVersion;
    header.kind = kSuperblockKind;
    header.headerLength = internal::kObjectHeaderSize;
    header.identity = identity;
    header.identity.deviceEpoch = state.deviceEpoch;
    header.granularity = state.granularityBytes;
    header.regionCount = state.regionCount;
    header.capacityBytes = state.capacityBytes;

    const std::vector<std::uint8_t> payload = encodePayload(state);
    header.payloadLength = static_cast<std::uint32_t>(payload.size());
    header.payloadCrc = crc32c(payload);

    std::vector<std::uint8_t> slot(kSuperblockSlotSize, 0u);
    const std::vector<std::uint8_t> headerBytes = internal::encodeObjectHeader(header);
    std::memcpy(slot.data(), headerBytes.data(), headerBytes.size());
    std::memcpy(slot.data() + headerBytes.size(), payload.data(), payload.size());

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return Status::IoError;
    }

    const off_t offset = static_cast<off_t>(slotIndex) * static_cast<off_t>(kSuperblockSlotSize);
    std::size_t written = 0;
    while (written < slot.size()) {
        const ssize_t result = ::pwrite(fd, slot.data() + written, slot.size() - written,
                                        offset + static_cast<off_t>(written));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return Status::IoError;
        }
        written += static_cast<std::size_t>(result);
    }

    const Status status = internal::syncFile(fd);
    ::close(fd);
    if (status != Status::Ok) {
        return status;
    }

    const std::size_t slash = path.find_last_of('/');
    const std::string directory = (slash == std::string::npos) ? "." : path.substr(0, slash);
    return internal::fsyncDirectory(directory);
}

} // namespace

Status superblockRead(const std::string& path,
                      const MetadataIdentity& identity,
                      SuperblockState& out,
                      bool& found)
{
    found = false;

    std::vector<std::uint8_t> contents;
    const Status readStatus = internal::readWholeFile(path, contents);
    if (readStatus == Status::NotFound) {
        return Status::Ok; // not created yet
    }
    if (readStatus != Status::Ok) {
        return readStatus;
    }
    if (contents.empty()) {
        return Status::Ok;
    }

    bool haveBest = false;
    SuperblockState best;

    for (std::uint32_t slot = 0; slot < kSlotCount; ++slot) {
        const std::size_t offset = static_cast<std::size_t>(slot) * kSuperblockSlotSize;
        if (contents.size() < offset + kSuperblockSlotSize) {
            continue;
        }

        internal::ObjectHeader header;
        SuperblockState candidate;
        if (!decodeSlot(contents.data() + offset, kSuperblockSlotSize, identity, header, candidate)) {
            continue;
        }

        if (!haveBest || candidate.sequence > best.sequence) {
            best = candidate;
            haveBest = true;
        }
    }

    if (!haveBest) {
        return Status::Corrupt;
    }

    out = best;
    found = true;
    return Status::Ok;
}

Status superblockWrite(const std::string& path,
                       const MetadataIdentity& identity,
                       SuperblockState& state)
{
    const std::uint64_t previousSequence = state.sequence;
    state.sequence = previousSequence + 1u;

    const std::uint32_t slotIndex = static_cast<std::uint32_t>(state.sequence % kSlotCount);
    return writeSlot(path, slotIndex, identity, state);
}

} // namespace blockchronicle
