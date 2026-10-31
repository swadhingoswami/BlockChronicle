// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/simulator.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>
#include <utility>

#include "persistence/file_io.h"

namespace blockchronicle {
namespace {

Status preadExact(int fd, void* buffer, std::size_t length, std::uint64_t offset)
{
    std::size_t done = 0;
    std::uint8_t* bytes = static_cast<std::uint8_t*>(buffer);
    while (done < length) {
        const ssize_t result = ::pread(fd, bytes + done, length - done,
                                       static_cast<off_t>(offset + done));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status::IoError;
        }
        if (result == 0) {
            return Status::OutOfRange;
        }
        done += static_cast<std::size_t>(result);
    }
    return Status::Ok;
}

Status pwriteExact(int fd, const void* buffer, std::size_t length, std::uint64_t offset)
{
    std::size_t done = 0;
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(buffer);
    while (done < length) {
        const ssize_t result = ::pwrite(fd, bytes + done, length - done,
                                        static_cast<off_t>(offset + done));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status::IoError;
        }
        done += static_cast<std::size_t>(result);
    }
    return Status::Ok;
}

Status copyFile(const std::string& source, const std::string& destination)
{
    const int input = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        return Status::IoError;
    }
    const int output = ::open(destination.c_str(),
                              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (output < 0) {
        ::close(input);
        return Status::IoError;
    }

    std::uint8_t buffer[64 * 1024];
    Status status = Status::Ok;

    for (;;) {
        const ssize_t readBytes = ::read(input, buffer, sizeof(buffer));
        if (readBytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            status = Status::IoError;
            break;
        }
        if (readBytes == 0) {
            break;
        }

        std::size_t done = 0;
        while (done < static_cast<std::size_t>(readBytes)) {
            const ssize_t written = ::write(output, buffer + done,
                                            static_cast<std::size_t>(readBytes) - done);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                status = Status::IoError;
                break;
            }
            done += static_cast<std::size_t>(written);
        }
        if (status != Status::Ok) {
            break;
        }
    }

    if (status == Status::Ok) {
        status = internal::syncFile(output);
    }

    ::close(input);
    ::close(output);
    return status;
}

} // namespace

SimulatorDevice::SimulatorDevice(std::string metadataDirectory,
                                 std::string dataPath,
                                 const MetadataIdentity& identity,
                                 const Geometry& geometry,
                                 CrashPoint& crash)
    : metadataDirectory_(std::move(metadataDirectory))
    , dataPath_(std::move(dataPath))
    , identity_(identity)
    , geometry_(geometry)
    , crash_(crash)
{
}

SimulatorDevice::~SimulatorDevice()
{
    if (dataFd_ >= 0) {
        ::close(dataFd_);
    }
}

std::string SimulatorDevice::snapshotDirectory() const
{
    return metadataDirectory_ + "/snapshots";
}

Status SimulatorDevice::ensureDataFile(std::uint64_t capacityBytes)
{
    const int fd = ::open(dataPath_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return Status::IoError;
    }

    struct stat info;
    if (::fstat(fd, &info) != 0) {
        ::close(fd);
        return Status::IoError;
    }

    if (static_cast<std::uint64_t>(info.st_size) != capacityBytes) {
        if (::ftruncate(fd, static_cast<off_t>(capacityBytes)) != 0) {
            ::close(fd);
            return Status::IoError;
        }
    }

    dataFd_ = fd;
    return Status::Ok;
}

Status SimulatorDevice::discardStaleSnapshots()
{
    std::vector<std::string> names;
    const Status status = internal::listDirectory(snapshotDirectory(), names);
    if (status == Status::NotFound) {
        return internal::ensureDirectory(snapshotDirectory());
    }
    if (status != Status::Ok) {
        return status;
    }

    for (std::size_t i = 0; i < names.size(); ++i) {
        internal::removeFile(snapshotDirectory() + "/" + names[i]);
    }
    snapshots_.clear();
    return Status::Ok;
}

Status SimulatorDevice::create(const std::string& metadataDirectory,
                               const std::string& dataPath,
                               const MetadataIdentity& identity,
                               std::uint64_t capacityBytes,
                               const DeviceOptions& options,
                               CrashPoint& crash,
                               std::unique_ptr<SimulatorDevice>& out)
{
    Geometry geometry;
    Status status = Geometry::make(capacityBytes, kDefaultGranularityBytes, geometry);
    if (status != Status::Ok) {
        return status;
    }

    std::unique_ptr<SimulatorDevice> device(
        new SimulatorDevice(metadataDirectory, dataPath, identity, geometry, crash));

    status = internal::ensureDirectory(metadataDirectory);
    if (status != Status::Ok) {
        return status;
    }
    status = internal::ensureDirectory(device->snapshotDirectory());
    if (status != Status::Ok) {
        return status;
    }
    status = device->ensureDataFile(capacityBytes);
    if (status != Status::Ok) {
        return status;
    }

    std::unique_ptr<PersistentDevice> metadata;
    status = PersistentDevice::create(metadataDirectory, identity, geometry, options, crash,
                                      metadata);
    if (status != Status::Ok) {
        return status;
    }
    device->metadata_ = std::move(metadata);

    out = std::move(device);
    return Status::Ok;
}

Status SimulatorDevice::open(const std::string& metadataDirectory,
                             const std::string& dataPath,
                             const MetadataIdentity& identity,
                             const DeviceOptions& options,
                             CrashPoint& crash,
                             std::unique_ptr<SimulatorDevice>& out)
{
    std::unique_ptr<PersistentDevice> metadata;
    Status status =
        PersistentDevice::open(metadataDirectory, identity, options, crash, metadata);
    if (status != Status::Ok) {
        return status;
    }

    Geometry geometry;
    status = Geometry::make(metadata->geometry().capacityBytes(),
                            metadata->geometry().granularityBytes(), geometry);
    if (status != Status::Ok) {
        return status;
    }

    std::unique_ptr<SimulatorDevice> device(new SimulatorDevice(
        metadataDirectory, dataPath, metadata->identity(), geometry, crash));
    device->metadata_ = std::move(metadata);

    status = device->ensureDataFile(geometry.capacityBytes());
    if (status != Status::Ok) {
        return status;
    }

    // A snapshot from a previous process cannot be trusted: its boundary may
    // already have been committed. Discard them all.
    status = device->discardStaleSnapshots();
    if (status != Status::Ok) {
        return status;
    }

    out = std::move(device);
    return Status::Ok;
}

Status SimulatorDevice::write(std::uint64_t offsetBytes,
                              std::uint64_t lengthBytes,
                              const void* data)
{
    if (lengthBytes == 0u) {
        return Status::Ok;
    }

    std::lock_guard<std::mutex> guard(dataMutex_);

    // Tracking fact durable first, then the data. A crash between the two leaves
    // a false positive, never a false negative.
    Status status = metadata_->markWrite(offsetBytes, lengthBytes);
    if (status != Status::Ok) {
        return status;
    }

    status = pwriteExact(dataFd_, data, lengthBytes, offsetBytes);
    if (status != Status::Ok) {
        return status;
    }

    crash_.hit("after_data_write");
    return Status::Ok;
}

Status SimulatorDevice::read(std::uint64_t offsetBytes,
                             std::uint64_t lengthBytes,
                             void* out) const
{
    if (offsetBytes > geometry_.capacityBytes()
        || lengthBytes > geometry_.capacityBytes() - offsetBytes) {
        return Status::OutOfRange;
    }
    if (lengthBytes == 0u) {
        return Status::Ok;
    }
    return preadExact(dataFd_, out, lengthBytes, offsetBytes);
}

Status SimulatorDevice::beginBackup(const ConsumerId& consumer,
                                    BackupSession& session,
                                    SnapshotHandle& snapshot)
{
    std::lock_guard<std::mutex> guard(dataMutex_);

    // Make the boundary cover everything written so far, then capture it and the
    // data together. No write can interleave: dataMutex_ is held throughout.
    // The first backup must seal unconditionally, otherwise there is no sealed
    // generation to serve as a boundary.
    Status status = (metadata_->lastSealedGeneration() == 0u) ? metadata_->seal()
                                                             : metadata_->sealIfDirty();
    if (status != Status::Ok) {
        return status;
    }

    status = metadata_->beginBackup(consumer, session);
    if (status != Status::Ok) {
        return status;
    }

    const std::uint64_t id = nextSnapshot_++;
    char name[64];
    std::snprintf(name, sizeof(name), "snap-%020llu.bin", static_cast<unsigned long long>(id));
    const std::string path = snapshotDirectory() + "/" + name;

    status = copyFile(dataPath_, path);
    if (status != Status::Ok) {
        metadata_->abortBackup(session);
        return status;
    }

    snapshots_[id] = path;
    snapshot.id = id;
    return Status::Ok;
}

Status SimulatorDevice::readSnapshot(const SnapshotHandle& snapshot,
                                     std::uint64_t offsetBytes,
                                     std::uint64_t lengthBytes,
                                     void* out) const
{
    const std::map<std::uint64_t, std::string>::const_iterator it =
        snapshots_.find(snapshot.id);
    if (it == snapshots_.end()) {
        return Status::NotFound;
    }
    if (offsetBytes > geometry_.capacityBytes()
        || lengthBytes > geometry_.capacityBytes() - offsetBytes) {
        return Status::OutOfRange;
    }
    if (lengthBytes == 0u) {
        return Status::Ok;
    }

    const int fd = ::open(it->second.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return Status::IoError;
    }
    const Status status = preadExact(fd, out, lengthBytes, offsetBytes);
    ::close(fd);
    return status;
}

Status SimulatorDevice::releaseSnapshot(const SnapshotHandle& snapshot)
{
    const std::map<std::uint64_t, std::string>::iterator it = snapshots_.find(snapshot.id);
    if (it == snapshots_.end()) {
        return Status::NotFound;
    }
    const Status status = internal::removeFile(it->second);
    snapshots_.erase(it);
    return status;
}

Status SimulatorDevice::seal()
{
    return metadata_->seal();
}

Status SimulatorDevice::sealIfDirty()
{
    return metadata_->sealIfDirty();
}

Status SimulatorDevice::checkpoint()
{
    return metadata_->checkpoint();
}

Status SimulatorDevice::collectGarbage(std::uint64_t keepGenerations, GcResult& out)
{
    return metadata_->collectGarbage(keepGenerations, out);
}

Status SimulatorDevice::close()
{
    std::lock_guard<std::mutex> guard(dataMutex_);
    for (std::map<std::uint64_t, std::string>::const_iterator it = snapshots_.begin();
         it != snapshots_.end();
         ++it) {
        internal::removeFile(it->second);
    }
    snapshots_.clear();
    return metadata_->close();
}

Status SimulatorDevice::addConsumer(const ConsumerId& id)
{
    return metadata_->addConsumer(id);
}

Status SimulatorDevice::getDirtyRanges(const BackupSession& session,
                                       std::uint64_t maxRanges,
                                       std::vector<DirtyRange>& out) const
{
    return metadata_->getDirtyRanges(session, maxRanges, out);
}

Status SimulatorDevice::commitBackup(BackupSession& session, BackupId newBackupId)
{
    return metadata_->commitBackup(session, newBackupId);
}

Status SimulatorDevice::commitBackupAt(const ConsumerId& id,
                                       BackupId newBackupId,
                                       Generation boundary)
{
    return metadata_->commitBackupAt(id, newBackupId, boundary);
}

Status SimulatorDevice::abortBackup(const BackupSession& session)
{
    return metadata_->abortBackup(session);
}

Status SimulatorDevice::consumerStatus(const ConsumerId& id, ConsumerStatus& out) const
{
    return metadata_->consumerStatus(id, out);
}

Status SimulatorDevice::compareGenerations(Generation from,
                                           Generation to,
                                           DirtyBitmap& out) const
{
    return metadata_->dirtySince(from, to, out);
}

Generation SimulatorDevice::lastSealedGeneration() const
{
    return metadata_->lastSealedGeneration();
}

Generation SimulatorDevice::activeGeneration() const
{
    return metadata_->activeGeneration();
}

RecoveryState SimulatorDevice::recoveryState() const
{
    return metadata_->recoveryState();
}

} // namespace blockchronicle
