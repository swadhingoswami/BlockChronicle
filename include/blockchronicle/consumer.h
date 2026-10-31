// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_CONSUMER_H
#define BLOCKCHRONICLE_CONSUMER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "blockchronicle/bitmap.h"
#include "blockchronicle/seal.h"
#include "blockchronicle/types.h"

namespace blockchronicle {

using ConsumerId = std::string;
using SessionId = std::uint64_t;

// A backup of a consumer that has never completed a baseline copies the whole
// device at the boundary. An incremental copies only the returned ranges.
enum class BackupKind : std::uint8_t {
    Full = 0,
    Incremental = 1,
};

// A consumer's open backup operation. The pending set is a snapshot taken at
// beginSession under the device's shared lock, so it corresponds exactly to the
// generation that was sealed at that instant.
struct BackupSession {
    ConsumerId consumerId;
    SessionId sessionId = 0;
    Generation cursorAtStart = 0;
    Generation boundary = 0;
    BackupKind kind = BackupKind::Full;
    SealedDelta pending; // empty and unused for a Full session
};

struct ConsumerStatus {
    ConsumerId consumerId;
    BackupId lastBackupId = 0;
    Generation cursor = 0;
    Generation cursorEpoch = 0;
    bool fullBackupRequired = true;
    bool sessionOpen = false;
    Generation openSessionBoundary = 0;
    // Deltas sealed since the cursor that are still pending. Zero after a commit.
    std::uint64_t unackedGenerations = 0;
};

// Tracks independent backup consumers of one device.
//
// Each consumer holds its own cursor (device epoch + generation) and its own
// pending bitmap: the OR of every delta sealed since that cursor. The pending
// bitmap is the correct acceleration for "changed since C" because it is
// parameterised by C; a device-wide cumulative cannot answer that question
// (docs/bitmap-history.md section 3.1).
//
// Pending is a cache. The deltas remain the source of truth: a consumer's
// unacked list holds shared references to them, which is also what allows a
// commit at an older boundary to be computed exactly. Bitwise OR cannot be
// subtracted, so committing at generation B when seals have continued to B+n
// requires rebuilding pending from the surviving deltas rather than clearing it.
//
// Thread safety: every public method is internally synchronised. The device
// coordinator (GenerationManager) holds the device's shared lock while calling
// beginSession, and the exclusive lock while calling onDeltaSealed.
class ConsumerManager {
public:
    static constexpr std::size_t kDefaultMaxConsumers = 10;
    static constexpr std::size_t kMaxConsumerIdLength = 128;

    ConsumerManager(const Geometry& geometry,
                    Generation deviceEpoch,
                    std::size_t maxConsumers = kDefaultMaxConsumers);

    ConsumerManager(const ConsumerManager&) = delete;
    ConsumerManager& operator=(const ConsumerManager&) = delete;
    ConsumerManager(ConsumerManager&&) = delete;
    ConsumerManager& operator=(ConsumerManager&&) = delete;

    // --- registration ----------------------------------------------------

    Status addConsumer(const ConsumerId& id);
    Status removeConsumer(const ConsumerId& id); // refuses while a session is open
    bool hasConsumer(const ConsumerId& id) const;
    std::size_t consumerCount() const;
    std::size_t maxConsumers() const { return maxConsumers_; }

    Generation deviceEpoch() const;

    // Device re-baseline: every cursor becomes unusable and every open session
    // is invalidated. Tracking correctness is preserved by forcing full backups.
    void invalidateAll();

    // --- session lifecycle -----------------------------------------------

    // boundary must be a sealed generation. A consumer requiring a baseline
    // gets a Full session; getDirtyRanges refuses it.
    Status beginSession(const ConsumerId& id, Generation boundary, BackupSession& out);

    // Advances the cursor to the session boundary. Idempotency is deliberately
    // not provided at this layer: the session is invalidated, so a retry fails
    // with InvalidState rather than double-advancing. Crash-safe idempotency is
    // a property of WAL replay (Phase 5), keyed by (consumer, backupId).
    Status commit(BackupSession& session, BackupId newBackupId);

    // Releases the session. Never advances anything.
    Status abort(const BackupSession& session);

    Status status(const ConsumerId& id, ConsumerStatus& out) const;

    // --- seal notification -----------------------------------------------

    // Called by the device coordinator after a delta is durable and published.
    // Consumers that require a baseline are skipped: their pending set is
    // meaningless until the baseline commits.
    void onDeltaSealed(Generation generation, const DirtyBitmap& delta);

private:
    struct Record {
        BackupId lastBackupId = 0;
        Generation cursor = 0;
        Generation cursorEpoch = 0;
        bool fullBackupRequired = true;
        bool sessionOpen = false;
        SessionId openSession = 0;
        Generation openSessionBoundary = 0;
        // Deltas sealed since cursor, ascending. Shared, not copied.
        std::deque<std::pair<Generation, SealedDelta>> unacked;
        SealedDelta pending;
    };

    Status validateId(const ConsumerId& id) const;
    void rebuildPendingLocked(Record& record);

    Geometry geometry_;
    Generation deviceEpoch_;
    std::size_t maxConsumers_;

    mutable std::mutex mutex_;
    std::map<ConsumerId, Record> consumers_;
    SessionId nextSessionId_ = 1;
};

// Converts a session's pending set into coalesced byte ranges.
//
// Returns Status::InvalidState for a Full session: the consumer must copy the
// whole device at the boundary, and the engine must not pretend an empty range
// list means "nothing changed".
Status getDirtyRanges(const BackupSession& session,
                      std::uint64_t maxRanges,
                      std::vector<DirtyRange>& out);

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_CONSUMER_H
