// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_PERSISTENT_DEVICE_H
#define BLOCKCHRONICLE_PERSISTENT_DEVICE_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "blockchronicle/bitmap.h"
#include "blockchronicle/consumer.h"
#include "blockchronicle/crash_point.h"
#include "blockchronicle/generation.h"
#include "blockchronicle/history.h"
#include "blockchronicle/identity.h"
#include "blockchronicle/seal.h"
#include "blockchronicle/superblock.h"
#include "blockchronicle/types.h"
#include "blockchronicle/wal.h"

namespace blockchronicle {

struct DeviceOptions {
    std::size_t maxConsumers = ConsumerManager::kDefaultMaxConsumers;

    // True when a tracking fact is durable before the write it describes is
    // acknowledged. The simulator satisfies this; a kernel backend must prove it
    // before claiming it.
    bool durableBeforeAck = true;

    // When true, an unclean shutdown invalidates tracking and forces full
    // backups. Required whenever durableBeforeAck is false, because the loss
    // window cannot be bounded.
    bool invalidateOnUncleanShutdown = false;

    // Journal every mark before acknowledging it. Costs one fsync per write and
    // is what makes durableBeforeAck honest for the simulator. A kernel backend
    // with in-kernel tracking sets this false and must then also set
    // durableBeforeAck false.
    bool journalMarks = true;
};

enum class RecoveryState : std::uint8_t {
    Created = 0,     // brand new metadata directory
    Clean = 1,       // previous close was graceful, history intact
    Replayed = 2,    // WAL replayed after an unclean stop, history intact
    Invalidated = 3, // history unusable; every consumer needs a full backup
};

// A single tracked device with durable metadata.
//
// On-disk layout:
//   <directory>/manifest.bin          two 4 KiB superblock slots
//   <directory>/history/gen-*.delta   one file per sealed generation
//   <directory>/wal/segment-*.wal     append-only control-plane log
//
// Durability rules, in the order the code enforces them:
//   1. A delta file is fsynced and renamed before its SEAL record is appended.
//   2. A SEAL record is fsynced before the generation becomes visible.
//   3. A consumer commit is appended and fsynced before the cursor moves.
//   4. The superblock is written last at checkpoint, and is the sole authority
//      for where WAL replay starts.
//   5. A record that is truncated or torn at the tail was never acknowledged, so
//      discarding it cannot lose an acknowledged fact.
class PersistentDevice final : public SealSink {
public:
    static Status create(const std::string& directory,
                         const MetadataIdentity& identity,
                         const Geometry& geometry,
                         const DeviceOptions& options,
                         CrashPoint& crash,
                         std::unique_ptr<PersistentDevice>& out);

    static Status open(const std::string& directory,
                       const MetadataIdentity& expectedIdentity,
                       const DeviceOptions& options,
                       CrashPoint& crash,
                       std::unique_ptr<PersistentDevice>& out);

    ~PersistentDevice() override;

    // --- SealSink --------------------------------------------------------

    Status commitDelta(Generation generation, const DirtyBitmap& delta) override;

    // --- tracking --------------------------------------------------------

    bool markRegion(std::uint32_t region);
    Status markWrite(std::uint64_t offsetBytes, std::uint64_t lengthBytes);
    Status seal();

    // --- consumers -------------------------------------------------------

    Status addConsumer(const ConsumerId& id);
    Status removeConsumer(const ConsumerId& id);
    Status beginBackup(const ConsumerId& id, BackupSession& out);
    Status getDirtyRanges(const BackupSession& session,
                          std::uint64_t maxRanges,
                          std::vector<DirtyRange>& out) const;
    Status commitBackup(BackupSession& session, BackupId newBackupId);
    Status abortBackup(const BackupSession& session);
    Status consumerStatus(const ConsumerId& id, ConsumerStatus& out) const;

    // --- lifecycle -------------------------------------------------------

    Status checkpoint();
    Status close();

    RecoveryState recoveryState() const { return recoveryState_; }
    Generation activeGeneration() const;
    Generation lastSealedGeneration() const;
    const Geometry& geometry() const { return geometry_; }
    const MetadataIdentity& identity() const { return identity_; }
    const ConsumerManager& consumers() const { return *consumers_; }

private:
    class ReplayHandler;

    PersistentDevice(std::string directory,
                     const MetadataIdentity& identity,
                     const Geometry& geometry,
                     const DeviceOptions& options,
                     CrashPoint& crash);

    Status initialiseFresh();
    Status initialiseFromCheckpoint();
    Status loadHistory();
    Status replayJournal();
    Status rebuildPending();
    Status writeSuperblock();
    Status checkpointInternal();
    Status applyReplayRecord(const WalRecord& record);
    Status journalMark(std::uint64_t offsetBytes, std::uint64_t lengthBytes);
    void invalidateTracking();

    std::string manifestPath() const;
    std::string historyPath(Generation generation) const;
    std::string walDirectory() const;

    std::string directory_;
    MetadataIdentity identity_;
    Geometry geometry_;
    DeviceOptions options_;
    CrashPoint& crash_;

    SuperblockState state_;
    RecoveryState recoveryState_ = RecoveryState::Created;

    std::unique_ptr<HistoryStore> history_;
    std::unique_ptr<ConsumerManager> consumers_;
    std::unique_ptr<GenerationManager> manager_;
    std::unique_ptr<WalWriter> wal_;

    bool replaying_ = false;
    bool closed_ = false;
    bool historyInvalid_ = false;
    // Marks replayed from the WAL whose generation is still active.
    std::vector<DirtyRange> pendingMarks_;
    std::uint64_t lastReplayedSequence_ = 0;
    std::mutex controlMutex_;
};

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_PERSISTENT_DEVICE_H
