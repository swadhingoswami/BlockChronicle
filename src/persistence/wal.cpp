// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/wal.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "blockchronicle/codec.h"
#include "blockchronicle/crc32c.h"
#include "file_io.h"

namespace blockchronicle {
namespace {

constexpr std::uint32_t kRecordMagic = 0x31524C57u; // "WLR1"
constexpr std::uint32_t kRecordHeaderSize = 32u;
constexpr std::uint32_t kRecordTrailerSize = 4u;
constexpr std::size_t kMaxPayload = 1u << 20; // 1 MiB: control records only

std::vector<std::uint8_t> encodeRecord(const WalRecord& record)
{
    ByteWriter writer;
    writer.u32(kRecordMagic);
    writer.u32(static_cast<std::uint32_t>(
        kRecordHeaderSize + record.payload.size() + kRecordTrailerSize));
    writer.u64(record.sequence);
    writer.u16(static_cast<std::uint16_t>(record.type));
    writer.u16(0u);
    writer.u64(record.generation);
    writer.u32(static_cast<std::uint32_t>(record.payload.size()));
    if (!record.payload.empty()) {
        writer.bytes(record.payload.data(), record.payload.size());
    }

    const std::uint32_t crc = crc32c(writer.data().data(), writer.size());
    writer.u32(crc);
    return writer.data();
}

bool parseSegmentName(const std::string& name, std::uint64_t& firstSequence)
{
    static const std::string prefix = "segment-";
    static const std::string suffix = ".wal";

    if (name.size() <= prefix.size() + suffix.size()) {
        return false;
    }
    if (name.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }

    const std::string digits = name.substr(prefix.size(),
                                           name.size() - prefix.size() - suffix.size());
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (digits[i] < '0' || digits[i] > '9') {
            return false;
        }
        value = value * 10u + static_cast<std::uint64_t>(digits[i] - '0');
    }
    firstSequence = value;
    return true;
}

} // namespace

std::string walSegmentFileName(std::uint64_t firstSequence)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "segment-%020llu.wal",
                  static_cast<unsigned long long>(firstSequence));
    return std::string(buffer);
}

WalWriter::~WalWriter()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Status WalWriter::open(const std::string& directory, std::uint64_t nextSequence)
{
    directory_ = directory;
    segmentStart_ = nextSequence;
    nextSequence_ = nextSequence;
    segmentPath_ = directory + "/" + walSegmentFileName(segmentStart_);

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    fd_ = ::open(segmentPath_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd_ < 0) {
        return Status::IoError;
    }
    return Status::Ok;
}

Status WalWriter::append(WalRecordType type,
                         std::uint64_t generation,
                         const std::vector<std::uint8_t>& payload)
{
    if (fd_ < 0) {
        return Status::InvalidState;
    }
    if (payload.size() > kMaxPayload) {
        return Status::InvalidArgument;
    }

    WalRecord record;
    record.sequence = nextSequence_;
    record.type = type;
    record.generation = generation;
    record.payload = payload;

    const std::vector<std::uint8_t> bytes = encodeRecord(record);

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(fd_, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status::IoError;
        }
        offset += static_cast<std::size_t>(written);
    }

    const Status syncStatus = internal::syncFile(fd_);
    if (syncStatus != Status::Ok) {
        return syncStatus;
    }

    ++nextSequence_;
    return Status::Ok;
}

Status WalWriter::rotate()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    segmentStart_ = nextSequence_;
    segmentPath_ = directory_ + "/" + walSegmentFileName(segmentStart_);

    fd_ = ::open(segmentPath_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd_ < 0) {
        return Status::IoError;
    }
    return internal::fsyncDirectory(directory_);
}

Status walListSegments(const std::string& directory,
                       std::vector<std::pair<std::uint64_t, std::string>>& segments)
{
    segments.clear();

    std::vector<std::string> names;
    const Status status = internal::listDirectory(directory, names);
    if (status != Status::Ok) {
        return status;
    }

    for (std::size_t i = 0; i < names.size(); ++i) {
        std::uint64_t firstSequence = 0;
        if (!parseSegmentName(names[i], firstSequence)) {
            continue;
        }
        segments.emplace_back(firstSequence, directory + "/" + names[i]);
    }

    std::sort(segments.begin(), segments.end());
    return Status::Ok;
}

Status walRemoveSegmentsBefore(const std::string& directory, std::uint64_t firstSequence)
{
    std::vector<std::pair<std::uint64_t, std::string>> segments;
    const Status status = walListSegments(directory, segments);
    if (status != Status::Ok) {
        return status;
    }

    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].first >= firstSequence) {
            continue;
        }
        const Status removeStatus = internal::removeFile(segments[i].second);
        if (removeStatus != Status::Ok) {
            return removeStatus;
        }
    }
    return Status::Ok;
}

WalReplaySummary replayWal(const std::string& directory,
                           std::uint64_t firstSequence,
                           WalHandler& handler)
{
    WalReplaySummary summary;

    std::vector<std::pair<std::uint64_t, std::string>> segments;
    const Status listStatus = walListSegments(directory, segments);
    if (listStatus != Status::Ok) {
        summary.status = (listStatus == Status::NotFound) ? Status::Ok : listStatus;
        return summary;
    }

    bool haveSequence = false;
    std::uint64_t expectedSequence = 0;

    for (std::size_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        const bool lastSegment = (segmentIndex + 1u == segments.size());

        std::vector<std::uint8_t> contents;
        if (internal::readWholeFile(segments[segmentIndex].second, contents) != Status::Ok) {
            summary.status = Status::IoError;
            return summary;
        }

        std::size_t offset = 0;
        while (offset < contents.size()) {
            const std::size_t remaining = contents.size() - offset;

            if (remaining < kRecordHeaderSize) {
                if (lastSegment) {
                    summary.tornTail = true;
                    summary.tornSegmentPath = segments[segmentIndex].second;
                    summary.tornSegmentValidLength = offset;
                    return summary;
                }
                summary.status = Status::Corrupt;
                return summary;
            }

            ByteReader reader(contents.data() + offset, kRecordHeaderSize);
            std::uint32_t magic = 0;
            std::uint32_t recordLength = 0;
            std::uint64_t sequence = 0;
            std::uint16_t type = 0;
            std::uint16_t flags = 0;
            std::uint64_t generation = 0;
            std::uint32_t payloadLength = 0;

            if (!reader.u32(magic) || !reader.u32(recordLength) || !reader.u64(sequence)
                || !reader.u16(type) || !reader.u16(flags) || !reader.u64(generation)
                || !reader.u32(payloadLength)) {
                summary.status = Status::Corrupt;
                return summary;
            }

            (void)flags;
            const bool plausible = magic == kRecordMagic
                && recordLength >= kRecordHeaderSize + kRecordTrailerSize
                && payloadLength == recordLength - kRecordHeaderSize - kRecordTrailerSize;

            if (!plausible) {
                // A garbage header is only a torn tail when it is the last thing
                // in the log. If whole records could still follow, the log is
                // corrupt and nothing after it can be trusted.
                if (lastSegment
                    && remaining <= kRecordHeaderSize + kRecordTrailerSize) {
                    summary.tornTail = true;
                    summary.tornSegmentPath = segments[segmentIndex].second;
                    summary.tornSegmentValidLength = offset;
                    return summary;
                }
                summary.status = Status::Corrupt;
                return summary;
            }

            if (recordLength > remaining) {
                if (lastSegment) {
                    summary.tornTail = true;
                    summary.tornSegmentPath = segments[segmentIndex].second;
                    summary.tornSegmentValidLength = offset;
                    return summary;
                }
                summary.status = Status::Corrupt;
                return summary;
            }

            const std::uint32_t storedCrc = static_cast<std::uint32_t>(
                contents[offset + recordLength - 4u])
                | (static_cast<std::uint32_t>(contents[offset + recordLength - 3u]) << 8u)
                | (static_cast<std::uint32_t>(contents[offset + recordLength - 2u]) << 16u)
                | (static_cast<std::uint32_t>(contents[offset + recordLength - 1u]) << 24u);

            if (crc32c(contents.data() + offset, recordLength - 4u) != storedCrc) {
                if (lastSegment && offset + recordLength == contents.size()) {
                    // A complete-looking final record with a bad checksum is a
                    // torn write: the length was written before the body landed.
                    summary.tornTail = true;
                    summary.tornSegmentPath = segments[segmentIndex].second;
                    summary.tornSegmentValidLength = offset;
                    return summary;
                }
                summary.status = Status::Corrupt;
                return summary;
            }

            offset += recordLength;

            if (sequence < firstSequence) {
                continue;
            }

            if (haveSequence && sequence != expectedSequence) {
                // A gap means records were lost, which no consumer can reason
                // about. Refuse rather than resume with a hole.
                summary.status = Status::Corrupt;
                return summary;
            }

            WalRecord record;
            record.sequence = sequence;
            record.type = static_cast<WalRecordType>(type);
            record.generation = generation;

            const std::size_t recordStart = offset - recordLength;
            const std::size_t payloadStart = recordStart + kRecordHeaderSize;
            record.payload.assign(contents.begin() + static_cast<std::ptrdiff_t>(payloadStart),
                                  contents.begin()
                                      + static_cast<std::ptrdiff_t>(payloadStart + payloadLength));

            const Status handlerStatus = handler.onRecord(record);
            if (handlerStatus != Status::Ok) {
                summary.status = handlerStatus;
                return summary;
            }

            haveSequence = true;
            expectedSequence = sequence + 1u;
            summary.lastSequence = sequence;
            ++summary.recordCount;
        }
    }

    return summary;
}

} // namespace blockchronicle
