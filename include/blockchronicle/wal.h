// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_WAL_H
#define BLOCKCHRONICLE_WAL_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blockchronicle/types.h"

namespace blockchronicle {

enum class WalRecordType : std::uint16_t {
    Seal = 1,
    ConsumerRegister = 2,
    ConsumerRemove = 3,
    ConsumerCommit = 4,
    Invalidate = 5,
    Checkpoint = 6,
    MarkRange = 7,
};

struct WalRecord {
    std::uint64_t sequence = 0;
    WalRecordType type = WalRecordType::Checkpoint;
    std::uint64_t generation = 0;
    std::vector<std::uint8_t> payload;
};

struct WalReplaySummary {
    Status status = Status::Ok;
    std::uint64_t lastSequence = 0;
    std::size_t recordCount = 0;
    // True when the log ended with a truncated or torn final record. That record
    // was never acknowledged (acknowledgement requires a completed fsync), so
    // discarding it cannot lose an acknowledged fact.
    bool tornTail = false;
    // When tornTail is set, the last segment is valid up to validLength and
    // recovery should truncate it there before appending new records.
    std::string tornSegmentPath;
    std::uint64_t tornSegmentValidLength = 0;
};

class WalHandler {
public:
    virtual ~WalHandler() = default;
    // Returning anything other than Ok stops replay and is reported as-is.
    virtual Status onRecord(const WalRecord& record) = 0;
};

// Append-only writer. Every append is fsynced before it returns, which is what
// makes "durable before acknowledged" true for the control plane.
class WalWriter {
public:
    WalWriter() = default;
    ~WalWriter();

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    // Creates (or opens) a segment starting at nextSequence.
    Status open(const std::string& directory, std::uint64_t nextSequence);

    Status append(WalRecordType type,
                  std::uint64_t generation,
                  const std::vector<std::uint8_t>& payload);

    // Starts a fresh segment. The previous one stays on disk until the caller
    // has checkpointed and removed it.
    Status rotate();

    std::uint64_t nextSequence() const { return nextSequence_; }
    std::uint64_t segmentStart() const { return segmentStart_; }

private:
    std::string directory_;
    std::string segmentPath_;
    std::uint64_t segmentStart_ = 1;
    std::uint64_t nextSequence_ = 1;
    int fd_ = -1;
};

// Replays every segment in ascending order, dispatching records with
// sequence >= firstSequence. Stops at the first gap or unreadable record.
WalReplaySummary replayWal(const std::string& directory,
                           std::uint64_t firstSequence,
                           WalHandler& handler);

Status walListSegments(const std::string& directory,
                       std::vector<std::pair<std::uint64_t, std::string>>& segments);

Status walRemoveSegmentsBefore(const std::string& directory, std::uint64_t firstSequence);

std::string walSegmentFileName(std::uint64_t firstSequence);

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_WAL_H
