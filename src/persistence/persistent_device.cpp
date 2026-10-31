// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/persistent_device.h"

#include <utility>

#include "blockchronicle/codec.h"
#include "blockchronicle/delta_file.h"
#include "persistence/file_io.h"

namespace blockchronicle {
namespace {

std::vector<std::uint8_t> encodeConsumerId(const ConsumerId& id)
{
    ByteWriter writer;
    writer.string16(id);
    return writer.data();
}

std::vector<std::uint8_t> encodeConsumerCommit(const BackupSession& session, BackupId backupId)
{
    ByteWriter writer;
    writer.string16(session.consumerId);
    writer.u64(backupId);
    writer.u64(session.boundary);
    writer.u64(session.cursorAtStart);
    return writer.data();
}

struct CommitPayload {
    std::string consumerId;
    BackupId backupId = 0;
    Generation boundary = 0;
    Generation cursorAtStart = 0;
};

bool decodeConsumerCommit(const std::vector<std::uint8_t>& payload, CommitPayload& out)
{
    ByteReader reader(payload.data(), payload.size());
    if (!reader.string16(out.consumerId) || !reader.u64(out.backupId)
        || !reader.u64(out.boundary) || !reader.u64(out.cursorAtStart)) {
        return false;
    }
    return reader.good();
}

std::vector<std::uint8_t> encodeMark(std::uint64_t offsetBytes, std::uint64_t lengthBytes)
{
    ByteWriter writer;
    writer.u64(offsetBytes);
    writer.u64(lengthBytes);
    return writer.data();
}

bool decodeMark(const std::vector<std::uint8_t>& payload, DirtyRange& out)
{
    ByteReader reader(payload.data(), payload.size());
    if (!reader.u64(out.offsetBytes) || !reader.u64(out.lengthBytes)) {
        return false;
    }
    return reader.good();
}

bool decodeConsumerId(const std::vector<std::uint8_t>& payload, ConsumerId& out)
{
    ByteReader reader(payload.data(), payload.size());
    return reader.string16(out) && reader.good();
}

} // namespace

// ---------------------------------------------------------------------------
// Replay handler
// ---------------------------------------------------------------------------

class PersistentDevice::ReplayHandler final : public WalHandler {
public:
    explicit ReplayHandler(PersistentDevice& device)
        : device_(device)
    {
    }

    Status onRecord(const WalRecord& record) override
    {
        return device_.applyReplayRecord(record);
    }

private:
    PersistentDevice& device_;
};

// ---------------------------------------------------------------------------
// Construction and recovery
// ---------------------------------------------------------------------------

PersistentDevice::PersistentDevice(std::string directory,
                                   const MetadataIdentity& identity,
                                   const Geometry& geometry,
                                   const DeviceOptions& options,
                                   CrashPoint& crash)
    : directory_(std::move(directory))
    , identity_(identity)
    , geometry_(geometry)
    , options_(options)
    , crash_(crash)
{
}

PersistentDevice::~PersistentDevice() = default;

std::string PersistentDevice::manifestPath() const
{
    return directory_ + "/manifest.bin";
}

std::string PersistentDevice::walDirectory() const
{
    return directory_ + "/wal";
}

std::string PersistentDevice::historyPath(Generation generation) const
{
    return directory_ + "/history/" + deltaFileName(generation);
}

Status PersistentDevice::create(const std::string& directory,
                                const MetadataIdentity& identity,
                                const Geometry& geometry,
                                const DeviceOptions& options,
                                CrashPoint& crash,
                                std::unique_ptr<PersistentDevice>& out)
{
    if (options.maxConsumers == 0u) {
        return Status::InvalidArgument;
    }
    if (!options.durableBeforeAck && !options.invalidateOnUncleanShutdown) {
        // Refuse the configuration that cannot be safe: a backend that may lose
        // the tail but is allowed to resume would report incomplete results.
        return Status::InvalidArgument;
    }
    if (geometry.regionCount() == 0u) {
        return Status::InvalidArgument;
    }

    std::unique_ptr<PersistentDevice> device(
        new PersistentDevice(directory, identity, geometry, options, crash));

    Status status = internal::ensureDirectory(directory);
    if (status != Status::Ok) {
        return status;
    }
    status = internal::ensureDirectory(directory + "/history");
    if (status != Status::Ok) {
        return status;
    }
    status = internal::ensureDirectory(device->walDirectory());
    if (status != Status::Ok) {
        return status;
    }
    if (internal::fileExists(device->manifestPath())) {
        return Status::InvalidState; // use open() for an existing store
    }

    SuperblockState state;
    state.deviceEpoch = identity.deviceEpoch;
    state.activeGeneration = 1;
    state.lastSealedGeneration = 0;
    state.oldestRetainedGeneration = 1;
    state.walFirstSequence = 1;
    state.capacityBytes = geometry.capacityBytes();
    state.granularityBytes = geometry.granularityBytes();
    state.regionCount = geometry.regionCount();
    state.trackingValid = true;
    state.cleanShutdown = false;
    state.durableBeforeAck = options.durableBeforeAck;

    status = superblockWrite(device->manifestPath(), identity, state);
    if (status != Status::Ok) {
        return status;
    }
    device->state_ = state;

    device->history_.reset(new HistoryStore(geometry, device.get()));
    device->consumers_.reset(
        new ConsumerManager(geometry, state.deviceEpoch, options.maxConsumers));
    device->manager_.reset(
        new GenerationManager(*device->history_, 1, device->consumers_.get()));
    device->wal_.reset(new WalWriter());
    status = device->wal_->open(device->walDirectory(), 1);
    if (status != Status::Ok) {
        return status;
    }

    device->recoveryState_ = RecoveryState::Created;
    out = std::move(device);
    return Status::Ok;
}

Status PersistentDevice::open(const std::string& directory,
                              const MetadataIdentity& expectedIdentity,
                              const DeviceOptions& options,
                              CrashPoint& crash,
                              std::unique_ptr<PersistentDevice>& out)
{
    if (options.maxConsumers == 0u) {
        return Status::InvalidArgument;
    }
    if (!options.durableBeforeAck && !options.invalidateOnUncleanShutdown) {
        return Status::InvalidArgument;
    }

    SuperblockState state;
    bool found = false;
    Status status = superblockRead(directory + "/manifest.bin", expectedIdentity, state, found);
    if (status != Status::Ok) {
        return status; // Corrupt: the caller quarantines, we do not guess
    }
    if (!found) {
        return Status::NotFound;
    }

    Geometry geometry;
    status = Geometry::make(state.capacityBytes, state.granularityBytes, geometry);
    if (status != Status::Ok) {
        return Status::Corrupt;
    }

    MetadataIdentity identity = expectedIdentity;
    identity.deviceEpoch = state.deviceEpoch;

    std::unique_ptr<PersistentDevice> device(
        new PersistentDevice(directory, identity, geometry, options, crash));
    device->state_ = state;

    device->history_.reset(new HistoryStore(geometry, device.get()));
    device->consumers_.reset(
        new ConsumerManager(geometry, state.deviceEpoch, options.maxConsumers));
    device->manager_.reset(new GenerationManager(*device->history_,
                                                 state.lastSealedGeneration + 1u,
                                                 device->consumers_.get()));

    // Consumers from the checkpoint.
    for (std::size_t i = 0; i < state.consumers.size(); ++i) {
        const PersistedConsumer& persisted = state.consumers[i];
        ConsumerSnapshot view;
        view.consumerId = persisted.consumerId;
        view.lastBackupId = persisted.lastBackupId;
        view.cursor = persisted.cursor;
        view.cursorEpoch = persisted.cursorEpoch;
        view.fullBackupRequired = persisted.fullBackupRequired;
        status = device->consumers_->restoreConsumer(view);
        if (status != Status::Ok) {
            return status;
        }
    }

    status = device->loadHistory();
    if (status != Status::Ok) {
        return status;
    }

    std::uint64_t lastReplayedSequence = 0;
    status = device->replayJournal();
    if (status != Status::Ok) {
        return status;
    }
    lastReplayedSequence = device->lastReplayedSequence_;

    status = device->rebuildPending();
    if (status != Status::Ok) {
        return status;
    }

    const bool unclean = !state.cleanShutdown;
    if (unclean && options.invalidateOnUncleanShutdown) {
        device->invalidateTracking();
    }

    // Arm the store: from this point a crash is unclean until close() says
    // otherwise.
    status = device->writeSuperblock();
    if (status != Status::Ok) {
        return status;
    }

    device->wal_.reset(new WalWriter());
    std::uint64_t nextSequence = state.walFirstSequence;
    if (lastReplayedSequence >= nextSequence) {
        nextSequence = lastReplayedSequence + 1u;
    }
    status = device->wal_->open(device->walDirectory(), nextSequence);
    if (status != Status::Ok) {
        return status;
    }

    if (device->recoveryState_ == RecoveryState::Invalidated
        || device->historyInvalid_) {
        device->recoveryState_ = RecoveryState::Invalidated;
    } else if (device->recoveryState_ == RecoveryState::Replayed || unclean) {
        device->recoveryState_ = RecoveryState::Replayed;
    } else {
        device->recoveryState_ = RecoveryState::Clean;
    }

    out = std::move(device);
    return Status::Ok;
}

Status PersistentDevice::loadHistory()
{
    replaying_ = true;

    for (Generation generation = state_.oldestRetainedGeneration;
         generation <= state_.lastSealedGeneration;
         ++generation) {
        DirtyBitmap delta(geometry_);
        Generation readGeneration = 0;
        const Status status = deltaRead(historyPath(generation), identity_, geometry_,
                                        readGeneration, delta);
        if (status != Status::Ok || readGeneration != generation) {
            // A hole in history cannot be answered around. Invalidate rather
            // than serve an incremental result with a gap.
            replaying_ = false;
            invalidateTracking();
            return Status::Ok;
        }

        const Status commit = history_->commitDelta(generation, delta);
        if (commit != Status::Ok) {
            replaying_ = false;
            invalidateTracking();
            return Status::Ok;
        }
    }

    replaying_ = false;
    return Status::Ok;
}

Status PersistentDevice::replayJournal()
{
    ReplayHandler handler(*this);
    const WalReplaySummary summary =
        replayWal(walDirectory(), state_.walFirstSequence, handler);

    if (summary.status != Status::Ok) {
        return summary.status;
    }

    if (summary.tornTail && !summary.tornSegmentPath.empty()) {
        // The final record was never acknowledged. Cut it off so that the next
        // recovery does not see it as a mid-log corruption.
        const Status truncate =
            internal::truncateFile(summary.tornSegmentPath, summary.tornSegmentValidLength);
        if (truncate != Status::Ok) {
            return truncate;
        }
    }

    lastReplayedSequence_ = summary.lastSequence;
    if (summary.recordCount != 0u) {
        recoveryState_ = RecoveryState::Replayed;
    }

    // Marks journaled for the generation that is still active must be restored,
    // otherwise a crash would lose writes that were already acknowledged.
    for (std::size_t i = 0; i < pendingMarks_.size(); ++i) {
        const Status marker = manager_->markWrite(pendingMarks_[i].offsetBytes,
                                                  pendingMarks_[i].lengthBytes);
        if (marker != Status::Ok) {
            return marker;
        }
    }
    pendingMarks_.clear();

    return Status::Ok;
}

Status PersistentDevice::applyReplayRecord(const WalRecord& record)
{
    switch (record.type) {
    case WalRecordType::Seal: {
        if (historyInvalid_) {
            return Status::Ok;
        }
        SealedDelta existing;
        if (history_->delta(record.generation, existing) == Status::Ok) {
            return Status::Ok; // already covered by the checkpointed history
        }

        DirtyBitmap delta(geometry_);
        Generation readGeneration = 0;
        const Status status = deltaRead(historyPath(record.generation), identity_, geometry_,
                                        readGeneration, delta);
        if (status != Status::Ok || readGeneration != record.generation) {
            invalidateTracking();
            return Status::Ok;
        }

        replaying_ = true;
        const Status commit = history_->commitDelta(record.generation, delta);
        replaying_ = false;
        if (commit != Status::Ok) {
            return commit;
        }
        state_.lastSealedGeneration = record.generation;
        return Status::Ok;
    }

    case WalRecordType::ConsumerRegister: {
        ConsumerId id;
        if (!decodeConsumerId(record.payload, id)) {
            return Status::Corrupt;
        }
        ConsumerSnapshot view;
        view.consumerId = id;
        view.cursorEpoch = identity_.deviceEpoch;
        view.fullBackupRequired = true;
        const Status status = consumers_->restoreConsumer(view);
        return (status == Status::InvalidArgument) ? Status::Ok : status;
    }

    case WalRecordType::ConsumerRemove: {
        ConsumerId id;
        if (!decodeConsumerId(record.payload, id)) {
            return Status::Corrupt;
        }
        const Status status = consumers_->removeConsumer(id);
        return (status == Status::NotFound) ? Status::Ok : status;
    }

    case WalRecordType::ConsumerCommit: {
        CommitPayload payload;
        if (!decodeConsumerCommit(record.payload, payload)) {
            return Status::Corrupt;
        }
        return consumers_->applyPersistedCommit(payload.consumerId, payload.backupId,
                                                payload.boundary);
    }

    case WalRecordType::Invalidate:
        consumers_->invalidateAll();
        history_->dropAll();
        historyInvalid_ = true;
        identity_.deviceEpoch = consumers_->deviceEpoch();
        state_.deviceEpoch = identity_.deviceEpoch;
        state_.trackingValid = false;
        return Status::Ok;

    case WalRecordType::MarkRange: {
        DirtyRange range;
        if (!decodeMark(record.payload, range)) {
            return Status::Corrupt;
        }
        // Marks for already sealed generations are covered by their delta files.
        if (record.generation == manager_->activeGeneration()) {
            pendingMarks_.push_back(range);
        }
        return Status::Ok;
    }

    case WalRecordType::Checkpoint:
        return Status::Ok;
    }

    return Status::Corrupt;
}

Status PersistentDevice::rebuildPending()
{
    const Generation oldest = history_->oldestGeneration();
    const Generation newest = history_->newestGeneration();
    if (oldest == 0u || newest == 0u) {
        return Status::Ok;
    }

    for (Generation generation = oldest; generation <= newest; ++generation) {
        SealedDelta delta;
        if (history_->delta(generation, delta) != Status::Ok) {
            return Status::Corrupt;
        }
        consumers_->onDeltaSealed(generation, *delta);
    }
    return Status::Ok;
}

void PersistentDevice::invalidateTracking()
{
    replaying_ = true;
    history_->dropAll();
    replaying_ = false;
    consumers_->invalidateAll();

    identity_.deviceEpoch = consumers_->deviceEpoch();
    state_.deviceEpoch = identity_.deviceEpoch;
    state_.trackingValid = false;
    state_.oldestRetainedGeneration = state_.lastSealedGeneration + 1u;
    historyInvalid_ = true;
    recoveryState_ = RecoveryState::Invalidated;
}

Status PersistentDevice::writeSuperblock()
{
    state_.capacityBytes = geometry_.capacityBytes();
    state_.granularityBytes = geometry_.granularityBytes();
    state_.regionCount = geometry_.regionCount();
    state_.deviceEpoch = identity_.deviceEpoch;
    state_.activeGeneration = manager_->activeGeneration();
    state_.lastSealedGeneration = manager_->lastSealedGeneration();

    return superblockWrite(manifestPath(), identity_, state_);
}

// ---------------------------------------------------------------------------
// SealSink
// ---------------------------------------------------------------------------

Status PersistentDevice::commitDelta(Generation generation, const DirtyBitmap& delta)
{
    if (replaying_) {
        return Status::Ok;
    }

    // 1. Delta payload durable.
    Status status = deltaWrite(historyPath(generation), identity_, generation, delta);
    if (status != Status::Ok) {
        return status;
    }
    crash_.hit("after_delta_write");

    // 2. Seal record durable. Only after this is the generation visible.
    const std::vector<std::uint8_t> empty;
    status = wal_->append(WalRecordType::Seal, generation, empty);
    if (status != Status::Ok) {
        return status;
    }
    crash_.hit("after_seal_wal");
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Tracking
// ---------------------------------------------------------------------------

Status PersistentDevice::journalMark(std::uint64_t offsetBytes, std::uint64_t lengthBytes)
{
    if (!options_.journalMarks) {
        return Status::Ok;
    }
    return wal_->append(WalRecordType::MarkRange, manager_->activeGeneration(),
                        encodeMark(offsetBytes, lengthBytes));
}

bool PersistentDevice::markRegion(std::uint32_t region)
{
    const std::uint64_t offset = geometry_.regionOffsetBytes(region);
    const std::uint64_t length = geometry_.regionLengthBytes(region);
    if (journalMark(offset, length) != Status::Ok) {
        return false;
    }
    return manager_->markRegion(region);
}

Status PersistentDevice::markWrite(std::uint64_t offsetBytes, std::uint64_t lengthBytes)
{
    if (lengthBytes == 0u) {
        return Status::Ok;
    }
    // Durable before acknowledged: the mark is in the log before it is applied,
    // so a crash between the two replays it (a false positive, never a loss).
    const Status status = journalMark(offsetBytes, lengthBytes);
    if (status != Status::Ok) {
        return status;
    }
    crash_.hit("after_mark_wal");
    return manager_->markWrite(offsetBytes, lengthBytes);
}

Status PersistentDevice::seal()
{
    std::lock_guard<std::mutex> guard(controlMutex_);
    if (closed_) {
        return Status::InvalidState;
    }
    return manager_->seal();
}

Status PersistentDevice::sealIfDirty()
{
    std::lock_guard<std::mutex> guard(controlMutex_);
    if (closed_) {
        return Status::InvalidState;
    }
    return manager_->sealIfDirtyAtLeast(1u);
}

Generation PersistentDevice::activeGeneration() const
{
    return manager_->activeGeneration();
}

Generation PersistentDevice::lastSealedGeneration() const
{
    return manager_->lastSealedGeneration();
}

// ---------------------------------------------------------------------------
// Consumers
// ---------------------------------------------------------------------------

Status PersistentDevice::addConsumer(const ConsumerId& id)
{
    std::lock_guard<std::mutex> guard(controlMutex_);
    if (closed_) {
        return Status::InvalidState;
    }

    Status status = consumers_->addConsumer(id);
    if (status != Status::Ok) {
        return status;
    }

    status = wal_->append(WalRecordType::ConsumerRegister, 0, encodeConsumerId(id));
    if (status != Status::Ok) {
        consumers_->removeConsumer(id); // roll back: nothing was acknowledged
        return status;
    }
    crash_.hit("after_consumer_register");
    return Status::Ok;
}

Status PersistentDevice::removeConsumer(const ConsumerId& id)
{
    std::lock_guard<std::mutex> guard(controlMutex_);
    if (closed_) {
        return Status::InvalidState;
    }

    ConsumerStatus current;
    Status status = consumers_->status(id, current);
    if (status != Status::Ok) {
        return status;
    }
    if (current.sessionOpen) {
        return Status::InvalidState;
    }

    status = wal_->append(WalRecordType::ConsumerRemove, 0, encodeConsumerId(id));
    if (status != Status::Ok) {
        return status;
    }
    crash_.hit("after_consumer_remove");
    return consumers_->removeConsumer(id);
}

Status PersistentDevice::beginBackup(const ConsumerId& id, BackupSession& out)
{
    std::lock_guard<std::mutex> guard(controlMutex_);
    if (closed_) {
        return Status::InvalidState;
    }
    return manager_->beginBackup(id, out);
}

Status PersistentDevice::getDirtyRanges(const BackupSession& session,
                                        std::uint64_t maxRanges,
                                        std::vector<DirtyRange>& out) const
{
    return blockchronicle::getDirtyRanges(session, maxRanges, out);
}

Status PersistentDevice::commitBackup(BackupSession& session, BackupId newBackupId)
{
    std::lock_guard<std::mutex> guard(controlMutex_);
    if (closed_) {
        return Status::InvalidState;
    }

    Status status = consumers_->validateCommit(session, newBackupId);
    if (status != Status::Ok) {
        return status;
    }

    // Durable before visible: the cursor may only move after this is fsynced.
    status = wal_->append(WalRecordType::ConsumerCommit, session.boundary,
                          encodeConsumerCommit(session, newBackupId));
    if (status != Status::Ok) {
        return status;
    }
    crash_.hit("after_consumer_commit");

    return consumers_->commit(session, newBackupId);
}

Status PersistentDevice::commitBackupAt(const ConsumerId& id,
                                        BackupId newBackupId,
                                        Generation boundary)
{
    std::lock_guard<std::mutex> guard(controlMutex_);
    if (closed_) {
        return Status::InvalidState;
    }

    ConsumerStatus current;
    Status status = consumers_->status(id, current);
    if (status != Status::Ok) {
        return status;
    }
    if (current.sessionOpen) {
        return Status::InvalidState; // finish the in-process session instead
    }
    if (boundary < current.cursor) {
        return Status::InvalidState; // cannot move a cursor backwards
    }
    if (newBackupId <= current.lastBackupId) {
        return Status::InvalidState;
    }

    ByteWriter writer;
    writer.string16(id);
    writer.u64(newBackupId);
    writer.u64(boundary);
    writer.u64(current.cursor);

    status = wal_->append(WalRecordType::ConsumerCommit, boundary, writer.data());
    if (status != Status::Ok) {
        return status;
    }
    crash_.hit("after_consumer_commit");

    return consumers_->applyPersistedCommit(id, newBackupId, boundary);
}

Status PersistentDevice::abortBackup(const BackupSession& session)
{
    std::lock_guard<std::mutex> guard(controlMutex_);
    return consumers_->abort(session);
}

Status PersistentDevice::consumerStatus(const ConsumerId& id, ConsumerStatus& out) const
{
    return consumers_->status(id, out);
}

// ---------------------------------------------------------------------------
// Checkpointing and shutdown
// ---------------------------------------------------------------------------

Status PersistentDevice::dirtySince(Generation fromExclusive,
                                    Generation toInclusive,
                                    DirtyBitmap& out) const
{
    return history_->unionRange(fromExclusive, toInclusive, out);
}

Status PersistentDevice::checkpoint()
{
    std::lock_guard<std::mutex> guard(controlMutex_);
    if (closed_) {
        return Status::InvalidState;
    }
    return checkpointInternal();
}

Status PersistentDevice::checkpointInternal()
{
    std::vector<ConsumerSnapshot> snapshot;
    Status status = consumers_->snapshot(snapshot);
    if (status != Status::Ok) {
        return status;
    }

    state_.consumers.clear();
    state_.consumers.reserve(snapshot.size());
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        PersistedConsumer persisted;
        persisted.consumerId = snapshot[i].consumerId;
        persisted.lastBackupId = snapshot[i].lastBackupId;
        persisted.cursor = snapshot[i].cursor;
        persisted.cursorEpoch = snapshot[i].cursorEpoch;
        persisted.fullBackupRequired = snapshot[i].fullBackupRequired;
        state_.consumers.push_back(persisted);
    }

    state_.activeGeneration = manager_->activeGeneration();
    state_.lastSealedGeneration = manager_->lastSealedGeneration();
    const Generation oldest = history_->oldestGeneration();
    state_.oldestRetainedGeneration =
        (oldest == 0u) ? state_.lastSealedGeneration + 1u : oldest;
    state_.cleanShutdown = false;
    state_.walFirstSequence = wal_->nextSequence();

    crash_.hit("before_superblock_write");

    // The superblock is written before the WAL is truncated: it is the sole
    // authority for where replay starts, so it must never point past records
    // that have not been captured in the history.
    status = writeSuperblock();
    if (status != Status::Ok) {
        return status;
    }
    crash_.hit("after_superblock_write");

    status = wal_->rotate();
    if (status != Status::Ok) {
        return status;
    }
    crash_.hit("after_wal_rotate");

    status = walRemoveSegmentsBefore(walDirectory(), state_.walFirstSequence);
    if (status != Status::Ok && status != Status::NotFound) {
        return status;
    }

    crash_.hit("after_checkpoint");
    return Status::Ok;
}

Status PersistentDevice::close()
{
    std::lock_guard<std::mutex> guard(controlMutex_);
    if (closed_) {
        return Status::Ok;
    }

    // Flush state, then record a clean shutdown. Any crash before this second
    // write leaves clean_shutdown = false, which recovery treats as unclean.
    Status status = checkpointInternal();
    if (status != Status::Ok) {
        return status;
    }

    state_.cleanShutdown = true;
    status = writeSuperblock();
    if (status != Status::Ok) {
        return status;
    }

    closed_ = true;
    return Status::Ok;
}

} // namespace blockchronicle
