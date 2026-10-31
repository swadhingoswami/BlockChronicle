// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/consumer.h"

#include <mutex>
#include <utility>

namespace blockchronicle {

ConsumerManager::ConsumerManager(const Geometry& geometry,
                                 Generation deviceEpoch,
                                 std::size_t maxConsumers)
    : geometry_(geometry)
    , deviceEpoch_(deviceEpoch)
    , maxConsumers_(maxConsumers)
{
}

Status ConsumerManager::validateId(const ConsumerId& id) const
{
    if (id.empty() || id.size() > kMaxConsumerIdLength) {
        return Status::InvalidArgument;
    }
    for (std::size_t i = 0; i < id.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(id[i]);
        if (c < 0x20u || c == 0x7Fu) {
            return Status::InvalidArgument;
        }
    }
    return Status::Ok;
}

Status ConsumerManager::addConsumer(const ConsumerId& id)
{
    const Status idStatus = validateId(id);
    if (idStatus != Status::Ok) {
        return idStatus;
    }

    std::lock_guard<std::mutex> guard(mutex_);

    if (consumers_.count(id) != 0u) {
        return Status::InvalidArgument;
    }
    if (consumers_.size() >= maxConsumers_) {
        return Status::OutOfRange;
    }

    Record record;
    record.cursorEpoch = deviceEpoch_;
    record.fullBackupRequired = true;
    record.pending = std::make_shared<const DirtyBitmap>(geometry_);

    consumers_.emplace(id, std::move(record));
    return Status::Ok;
}

Status ConsumerManager::removeConsumer(const ConsumerId& id)
{
    std::lock_guard<std::mutex> guard(mutex_);

    const std::map<ConsumerId, Record>::iterator it = consumers_.find(id);
    if (it == consumers_.end()) {
        return Status::NotFound;
    }
    if (it->second.sessionOpen) {
        return Status::InvalidState;
    }

    consumers_.erase(it);
    return Status::Ok;
}

bool ConsumerManager::hasConsumer(const ConsumerId& id) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return consumers_.count(id) != 0u;
}

std::size_t ConsumerManager::consumerCount() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return consumers_.size();
}

Generation ConsumerManager::deviceEpoch() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return deviceEpoch_;
}

void ConsumerManager::invalidateAll()
{
    std::lock_guard<std::mutex> guard(mutex_);

    ++deviceEpoch_;

    for (std::map<ConsumerId, Record>::iterator it = consumers_.begin();
         it != consumers_.end();
         ++it) {
        Record& record = it->second;
        record.cursorEpoch = deviceEpoch_ - 1u; // deliberately stale
        record.fullBackupRequired = true;
        record.sessionOpen = false;
        record.openSession = 0;
        record.openSessionBoundary = 0;
        record.unacked.clear();
        record.pending = std::make_shared<const DirtyBitmap>(geometry_);
    }
}

Status ConsumerManager::markFullBackupRequired(const ConsumerId& id)
{
    std::lock_guard<std::mutex> guard(mutex_);

    const std::map<ConsumerId, Record>::iterator it = consumers_.find(id);
    if (it == consumers_.end()) {
        return Status::NotFound;
    }

    Record& record = it->second;
    record.fullBackupRequired = true;
    record.sessionOpen = false;
    record.openSession = 0;
    record.openSessionBoundary = 0;
    record.unacked.clear();
    record.pending = std::make_shared<const DirtyBitmap>(geometry_);
    return Status::Ok;
}

Status ConsumerManager::beginSession(const ConsumerId& id,
                                     Generation boundary,
                                     BackupSession& out)
{
    std::lock_guard<std::mutex> guard(mutex_);

    const std::map<ConsumerId, Record>::iterator it = consumers_.find(id);
    if (it == consumers_.end()) {
        return Status::NotFound;
    }

    Record& record = it->second;

    if (record.sessionOpen) {
        return Status::InvalidState;
    }
    if (boundary == 0u) {
        // Nothing has ever been sealed; there is no consistent boundary.
        return Status::InvalidState;
    }
    if (!record.fullBackupRequired && boundary < record.cursor) {
        return Status::InvalidState;
    }

    BackupSession session;
    session.consumerId = id;
    session.sessionId = nextSessionId_++;
    session.cursorAtStart = record.cursor;
    session.boundary = boundary;
    session.kind = record.fullBackupRequired ? BackupKind::Full : BackupKind::Incremental;
    session.pending = record.fullBackupRequired
        ? std::make_shared<const DirtyBitmap>(geometry_)
        : record.pending;

    record.sessionOpen = true;
    record.openSession = session.sessionId;
    record.openSessionBoundary = boundary;

    out = session;
    return Status::Ok;
}

Status ConsumerManager::validateCommit(const BackupSession& session,
                                       BackupId newBackupId) const
{
    std::lock_guard<std::mutex> guard(mutex_);

    const std::map<ConsumerId, Record>::const_iterator it = consumers_.find(session.consumerId);
    if (it == consumers_.end()) {
        return Status::NotFound;
    }

    const Record& record = it->second;

    if (!record.sessionOpen || record.openSession != session.sessionId
        || session.sessionId == 0u) {
        return Status::InvalidState;
    }
    if (record.fullBackupRequired && session.kind != BackupKind::Full) {
        return Status::InvalidState;
    }
    if (newBackupId <= record.lastBackupId) {
        return Status::InvalidState;
    }
    return Status::Ok;
}

Status ConsumerManager::commit(BackupSession& session, BackupId newBackupId)
{
    std::lock_guard<std::mutex> guard(mutex_);

    const std::map<ConsumerId, Record>::iterator it = consumers_.find(session.consumerId);
    if (it == consumers_.end()) {
        return Status::NotFound;
    }

    Record& record = it->second;

    if (!record.sessionOpen || record.openSession != session.sessionId
        || session.sessionId == 0u) {
        return Status::InvalidState;
    }
    if (record.fullBackupRequired && session.kind != BackupKind::Full) {
        return Status::InvalidState;
    }
    if (newBackupId <= record.lastBackupId) {
        return Status::InvalidState;
    }

    // Drop the acknowledged deltas. Everything at or below the boundary is
    // covered by the backup that just succeeded -- by the full copy for a Full
    // session, by the transferred ranges for an Incremental one. Anything sealed
    // after the boundary stays pending, which is why this is a rebuild and not a
    // clear: OR is not invertible.
    while (!record.unacked.empty() && record.unacked.front().first <= session.boundary) {
        record.unacked.pop_front();
    }
    rebuildPendingLocked(record);

    record.cursor = session.boundary;
    record.cursorEpoch = deviceEpoch_;
    record.lastBackupId = newBackupId;
    record.fullBackupRequired = false;
    record.sessionOpen = false;
    record.openSession = 0;
    record.openSessionBoundary = 0;

    // Invalidate the caller's handle so a stale session cannot be committed or
    // aborted twice.
    session.sessionId = 0;
    return Status::Ok;
}

Status ConsumerManager::abort(const BackupSession& session)
{
    std::lock_guard<std::mutex> guard(mutex_);

    const std::map<ConsumerId, Record>::iterator it = consumers_.find(session.consumerId);
    if (it == consumers_.end()) {
        return Status::NotFound;
    }

    Record& record = it->second;
    if (!record.sessionOpen || record.openSession != session.sessionId
        || session.sessionId == 0u) {
        return Status::InvalidState;
    }

    record.sessionOpen = false;
    record.openSession = 0;
    record.openSessionBoundary = 0;
    return Status::Ok;
}

Status ConsumerManager::status(const ConsumerId& id, ConsumerStatus& out) const
{
    std::lock_guard<std::mutex> guard(mutex_);

    const std::map<ConsumerId, Record>::const_iterator it = consumers_.find(id);
    if (it == consumers_.end()) {
        return Status::NotFound;
    }

    const Record& record = it->second;

    ConsumerStatus result;
    result.consumerId = id;
    result.lastBackupId = record.lastBackupId;
    result.cursor = record.cursor;
    result.cursorEpoch = record.cursorEpoch;
    result.fullBackupRequired = record.fullBackupRequired
        || record.cursorEpoch != deviceEpoch_;
    result.sessionOpen = record.sessionOpen;
    result.openSessionBoundary = record.openSessionBoundary;
    result.unackedGenerations = static_cast<std::uint64_t>(record.unacked.size());

    out = result;
    return Status::Ok;
}

Status ConsumerManager::restoreConsumer(const ConsumerSnapshot& view)
{
    std::lock_guard<std::mutex> guard(mutex_);

    if (consumers_.count(view.consumerId) != 0u) {
        return Status::InvalidArgument;
    }
    if (consumers_.size() >= maxConsumers_) {
        return Status::OutOfRange;
    }

    Record record;
    record.lastBackupId = view.lastBackupId;
    record.cursor = view.cursor;
    record.cursorEpoch = view.cursorEpoch;
    record.fullBackupRequired = view.fullBackupRequired;
    record.pending = std::make_shared<const DirtyBitmap>(geometry_);

    consumers_.emplace(view.consumerId, std::move(record));
    return Status::Ok;
}

Status ConsumerManager::applyPersistedCommit(const ConsumerId& id,
                                             BackupId backupId,
                                             Generation boundary)
{
    std::lock_guard<std::mutex> guard(mutex_);

    const std::map<ConsumerId, Record>::iterator it = consumers_.find(id);
    if (it == consumers_.end()) {
        return Status::NotFound;
    }

    Record& record = it->second;
    if (backupId <= record.lastBackupId) {
        return Status::Ok; // already applied
    }

    record.lastBackupId = backupId;
    record.cursor = boundary;
    record.cursorEpoch = deviceEpoch_;
    record.fullBackupRequired = false;
    record.sessionOpen = false;
    record.openSession = 0;
    record.openSessionBoundary = 0;
    record.unacked.clear();
    record.pending = std::make_shared<const DirtyBitmap>(geometry_);
    return Status::Ok;
}

Status ConsumerManager::snapshot(std::vector<ConsumerSnapshot>& out) const
{
    std::lock_guard<std::mutex> guard(mutex_);

    out.clear();
    out.reserve(consumers_.size());
    for (std::map<ConsumerId, Record>::const_iterator it = consumers_.begin();
         it != consumers_.end();
         ++it) {
        ConsumerSnapshot view;
        view.consumerId = it->first;
        view.lastBackupId = it->second.lastBackupId;
        view.cursor = it->second.cursor;
        view.cursorEpoch = it->second.cursorEpoch;
        view.fullBackupRequired = it->second.fullBackupRequired
            || it->second.cursorEpoch != deviceEpoch_;
        out.push_back(view);
    }
    return Status::Ok;
}

Status ConsumerManager::list(std::vector<ConsumerStatus>& out) const
{
    std::lock_guard<std::mutex> guard(mutex_);

    out.clear();
    out.reserve(consumers_.size());
    for (std::map<ConsumerId, Record>::const_iterator it = consumers_.begin();
         it != consumers_.end();
         ++it) {
        const Record& record = it->second;
        ConsumerStatus entry;
        entry.consumerId = it->first;
        entry.lastBackupId = record.lastBackupId;
        entry.cursor = record.cursor;
        entry.cursorEpoch = record.cursorEpoch;
        entry.fullBackupRequired = record.fullBackupRequired
            || record.cursorEpoch != deviceEpoch_;
        entry.sessionOpen = record.sessionOpen;
        entry.openSessionBoundary = record.openSessionBoundary;
        entry.unackedGenerations = static_cast<std::uint64_t>(record.unacked.size());
        out.push_back(entry);
    }
    return Status::Ok;
}

void ConsumerManager::onDeltaSealed(Generation generation, const DirtyBitmap& delta)
{
    std::lock_guard<std::mutex> guard(mutex_);

    for (std::map<ConsumerId, Record>::iterator it = consumers_.begin();
         it != consumers_.end();
         ++it) {
        Record& record = it->second;

        if (record.fullBackupRequired) {
            // The pending set is meaningless until a baseline commits; the
            // commit will start it from empty at that point.
            continue;
        }
        if (record.cursorEpoch != deviceEpoch_) {
            continue;
        }
        if (generation <= record.cursor) {
            continue; // already covered by the consumer's last backup
        }

        record.unacked.emplace_back(generation, std::make_shared<const DirtyBitmap>(delta));

        DirtyBitmap merged(geometry_);
        const Status status = DirtyBitmap::bitwiseOr(*record.pending, delta, merged);
        if (status != Status::Ok) {
            // Geometry mismatch is impossible here: both bitmaps come from this
            // device. Treat it as a programming error, not a recoverable one.
            record.fullBackupRequired = true;
            continue;
        }
        record.pending = std::make_shared<const DirtyBitmap>(std::move(merged));
    }
}

void ConsumerManager::rebuildPendingLocked(Record& record)
{
    std::shared_ptr<const DirtyBitmap> rebuilt =
        std::make_shared<const DirtyBitmap>(geometry_);

    for (std::size_t i = 0; i < record.unacked.size(); ++i) {
        DirtyBitmap merged(geometry_);
        const Status status = DirtyBitmap::bitwiseOr(*rebuilt, *record.unacked[i].second, merged);
        if (status != Status::Ok) {
            rebuilt = std::make_shared<const DirtyBitmap>(geometry_);
            break;
        }
        rebuilt = std::make_shared<const DirtyBitmap>(std::move(merged));
    }

    record.pending = rebuilt;
}

Status getDirtyRanges(const BackupSession& session,
                      std::uint64_t maxRanges,
                      std::vector<DirtyRange>& out)
{
    if (session.sessionId == 0u) {
        return Status::InvalidState; // committed, aborted, or never created
    }
    if (session.kind == BackupKind::Full) {
        return Status::InvalidState;
    }
    if (session.pending == nullptr) {
        return Status::InvalidState;
    }
    return session.pending->toRanges(maxRanges, out);
}

} // namespace blockchronicle
