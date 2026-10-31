// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_SIMULATOR_H
#define BLOCKCHRONICLE_SIMULATOR_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "blockchronicle/bitmap.h"
#include "blockchronicle/consumer.h"
#include "blockchronicle/crash_point.h"
#include "blockchronicle/identity.h"
#include "blockchronicle/persistent_device.h"
#include "blockchronicle/types.h"

namespace blockchronicle {

struct SnapshotHandle {
    std::uint64_t id = 0;
};

// A file-backed virtual block device with real metadata underneath.
//
// This is a test and demonstration backend, not a substitute for kernel
// tracking. Its purpose is to make the whole pipeline exercisable end to end:
// write, seal, back up, restore, compare.
//
// Durability ordering follows the conservative model from the design:
//
//     journal the mark -> mark durable -> write the data -> fsync -> return
//
// Snapshots are a full copy of the data file taken under the same lock that
// serialises writes, so a snapshot always corresponds exactly to its boundary.
// That is O(capacity) and is only acceptable for test-sized devices; a real
// backend uses dm-snapshot or dm-thin.
class SimulatorDevice {
public:
    static Status create(const std::string& metadataDirectory,
                         const std::string& dataPath,
                         const MetadataIdentity& identity,
                         std::uint64_t capacityBytes,
                         const DeviceOptions& options,
                         CrashPoint& crash,
                         std::unique_ptr<SimulatorDevice>& out);

    static Status open(const std::string& metadataDirectory,
                       const std::string& dataPath,
                       const MetadataIdentity& identity,
                       const DeviceOptions& options,
                       CrashPoint& crash,
                       std::unique_ptr<SimulatorDevice>& out);

    ~SimulatorDevice();

    SimulatorDevice(const SimulatorDevice&) = delete;
    SimulatorDevice& operator=(const SimulatorDevice&) = delete;

    // --- data path -------------------------------------------------------

    Status write(std::uint64_t offsetBytes, std::uint64_t lengthBytes, const void* data);
    Status read(std::uint64_t offsetBytes, std::uint64_t lengthBytes, void* out) const;

    std::uint64_t capacityBytes() const { return geometry_.capacityBytes(); }
    const Geometry& geometry() const { return geometry_; }

    // --- backup ----------------------------------------------------------

    // Seals the active generation if it is dirty, captures the boundary, and
    // copies the data file. Writes are excluded for the whole operation, so the
    // snapshot is exactly the state at the boundary.
    Status beginBackup(const ConsumerId& consumer,
                       BackupSession& session,
                       SnapshotHandle& snapshot);

    Status readSnapshot(const SnapshotHandle& snapshot,
                        std::uint64_t offsetBytes,
                        std::uint64_t lengthBytes,
                        void* out) const;

    Status releaseSnapshot(const SnapshotHandle& snapshot);

    // --- metadata pass-through -------------------------------------------

    Status seal();
    Status sealIfDirty();
    Status checkpoint();
    Status collectGarbage(std::uint64_t keepGenerations, GcResult& out);
    Status close();

    Status addConsumer(const ConsumerId& id);
    Status getDirtyRanges(const BackupSession& session,
                          std::uint64_t maxRanges,
                          std::vector<DirtyRange>& out) const;
    Status commitBackup(BackupSession& session, BackupId newBackupId);
    Status commitBackupAt(const ConsumerId& id, BackupId newBackupId, Generation boundary);
    Status abortBackup(const BackupSession& session);
    Status consumerStatus(const ConsumerId& id, ConsumerStatus& out) const;
    Status listConsumers(std::vector<ConsumerStatus>& out) const;
    Status removeConsumer(const ConsumerId& id);

    Status saveSnapshot(Generation generation, const std::string& label);
    Status listSnapshots(std::vector<std::string>& labels) const;
    Status deleteSnapshot(const std::string& label);
    Status compareGenerations(Generation from, Generation to, DirtyBitmap& out) const;

    const MetadataIdentity& identity() const { return identity_; }
    Generation lastSealedGeneration() const;
    Generation activeGeneration() const;
    RecoveryState recoveryState() const;

private:
    SimulatorDevice(std::string metadataDirectory,
                    std::string dataPath,
                    const MetadataIdentity& identity,
                    const Geometry& geometry,
                    CrashPoint& crash);

    Status ensureDataFile(std::uint64_t capacityBytes);
    Status discardStaleSnapshots();
    std::string snapshotDirectory() const;

    std::string metadataDirectory_;
    std::string dataPath_;
    MetadataIdentity identity_;
    Geometry geometry_;
    CrashPoint& crash_;

    std::unique_ptr<PersistentDevice> metadata_;

    // Serialises writes against snapshot creation. A single mutex is deliberate:
    // the simulator is I/O bound and correctness of the copy matters more than
    // simulated parallelism.
    mutable std::mutex dataMutex_;
    int dataFd_ = -1;

    std::map<std::uint64_t, std::string> snapshots_;
    std::uint64_t nextSnapshot_{1};
};

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_SIMULATOR_H
