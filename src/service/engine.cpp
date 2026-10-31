// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/engine.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "persistence/file_io.h"

namespace blockchronicle {
namespace {

std::uint64_t fnv1a(const std::string& text)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < text.size(); ++i) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(text[i]));
        hash *= 1099511628211ull;
    }
    return hash;
}

bool validDeviceId(const std::string& id)
{
    if (id.empty() || id.size() > 64u) {
        return false;
    }
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

} // namespace

Engine::Engine(std::string rootDirectory)
    : root_(std::move(rootDirectory))
{
}

std::string Engine::registryPath() const
{
    return root_ + "/registry.db";
}

std::string Engine::deviceDirectory(const std::string& deviceId) const
{
    return root_ + "/devices/" + deviceId;
}

MetadataIdentity Engine::identityFor(const std::string& deviceId)
{
    // Deterministic so that a re-registration of the same id sees the same
    // device. The metadata uuid is derived separately so a copied directory is
    // still detectable.
    MetadataIdentity identity = MetadataIdentity::fromSeed(fnv1a(deviceId));
    identity.deviceEpoch = 1;
    return identity;
}

MetadataIdentity Engine::identityOf(const std::string& deviceId)
{
    return identityFor(deviceId);
}

Status Engine::initialise()
{
    std::lock_guard<std::mutex> guard(mutex_);

    Status status = internal::ensureDirectory(root_);
    if (status != Status::Ok) {
        return status;
    }
    status = internal::ensureDirectory(root_ + "/devices");
    if (status != Status::Ok) {
        return status;
    }
    return loadRegistry();
}

Status Engine::loadRegistry()
{
    records_.clear();

    std::ifstream input(registryPath());
    if (!input) {
        return Status::Ok; // empty registry
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::istringstream fields(line);
        DeviceRecord record;
        std::string capacity;
        if (!std::getline(fields, record.deviceId, '|')
            || !std::getline(fields, record.metadataDirectory, '|')
            || !std::getline(fields, record.dataPath, '|')
            || !std::getline(fields, capacity)) {
            return Status::Corrupt;
        }
        if (!validDeviceId(record.deviceId)) {
            return Status::Corrupt;
        }
        record.capacityBytes = std::strtoull(capacity.c_str(), nullptr, 10);
        records_[record.deviceId] = record;
    }

    return Status::Ok;
}

Status Engine::saveRegistry()
{
    std::vector<std::uint8_t> bytes;
    const std::string header = "# blockchronicle registry v1\n";
    bytes.insert(bytes.end(), header.begin(), header.end());

    for (std::map<std::string, DeviceRecord>::const_iterator it = records_.begin();
         it != records_.end();
         ++it) {
        std::ostringstream line;
        line << it->second.deviceId << '|' << it->second.metadataDirectory << '|'
             << it->second.dataPath << '|' << it->second.capacityBytes << '\n';
        const std::string text = line.str();
        bytes.insert(bytes.end(), text.begin(), text.end());
    }

    return internal::writeFileAtomic(registryPath(), bytes);
}

Status Engine::registerSimulatorDevice(const std::string& deviceId,
                                       const std::string& dataPath,
                                       std::uint64_t capacityBytes)
{
    if (!validDeviceId(deviceId)) {
        return Status::InvalidArgument;
    }
    if (capacityBytes == 0u) {
        return Status::InvalidArgument;
    }

    std::lock_guard<std::mutex> guard(mutex_);

    if (records_.count(deviceId) != 0u) {
        return Status::InvalidState;
    }

    DeviceRecord record;
    record.deviceId = deviceId;
    record.metadataDirectory = deviceDirectory(deviceId);
    record.dataPath = dataPath;
    record.capacityBytes = capacityBytes;

    // Checked on the filesystem rather than through device(), which would take
    // this mutex again.
    std::error_code error;
    if (std::filesystem::exists(record.metadataDirectory + "/manifest.bin", error)) {
        return Status::InvalidState; // already registered on disk
    }

    std::unique_ptr<SimulatorDevice> deviceHandle;
    Status status = SimulatorDevice::create(record.metadataDirectory, record.dataPath,
                                     identityFor(deviceId), capacityBytes, DeviceOptions{},
                                     crash_, deviceHandle);
    if (status != Status::Ok) {
        return status;
    }

    open_[deviceId] = std::move(deviceHandle);
    records_[deviceId] = record;
    return saveRegistry();
}

Status Engine::device(const std::string& deviceId, SimulatorDevice*& out)
{
    std::lock_guard<std::mutex> guard(mutex_);
    out = nullptr;

    const std::map<std::string, std::unique_ptr<SimulatorDevice>>::iterator open =
        open_.find(deviceId);
    if (open != open_.end()) {
        out = open->second.get();
        return Status::Ok;
    }

    const std::map<std::string, DeviceRecord>::const_iterator record =
        records_.find(deviceId);
    if (record == records_.end()) {
        return Status::NotFound;
    }

    std::unique_ptr<SimulatorDevice> handle;
    const Status status = SimulatorDevice::open(record->second.metadataDirectory,
                                                record->second.dataPath,
                                                identityFor(deviceId), options_,
                                                crash_, handle);
    if (status != Status::Ok) {
        if (logger_ != nullptr) {
            logger_->log(LogLevel::Error, "engine",
                         "event=device_open device=" + deviceId
                             + " result=" + toString(status));
        }
        return status;
    }

    if (logger_ != nullptr) {
        const char* state = "clean";
        switch (handle->recoveryState()) {
        case RecoveryState::Created: state = "created"; break;
        case RecoveryState::Clean: state = "clean"; break;
        case RecoveryState::Replayed: state = "replayed"; break;
        case RecoveryState::Invalidated: state = "invalidated"; break;
        }
        logger_->log(LogLevel::Info, "engine",
                     "event=device_open device=" + deviceId + " recovery=" + state);
    }

    out = handle.get();
    open_[deviceId] = std::move(handle);
    return Status::Ok;
}

Status Engine::removeDevice(const std::string& deviceId)
{
    std::lock_guard<std::mutex> guard(mutex_);

    const std::map<std::string, DeviceRecord>::iterator record = records_.find(deviceId);
    if (record == records_.end()) {
        return Status::NotFound;
    }

    const std::map<std::string, std::unique_ptr<SimulatorDevice>>::iterator open =
        open_.find(deviceId);
    if (open != open_.end()) {
        open->second->close();
        open_.erase(open);
    }

    std::error_code error;
    std::filesystem::remove_all(record->second.metadataDirectory, error);

    records_.erase(record);
    return saveRegistry();
}

std::vector<DeviceRecord> Engine::records() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<DeviceRecord> result;
    result.reserve(records_.size());
    for (std::map<std::string, DeviceRecord>::const_iterator it = records_.begin();
         it != records_.end();
         ++it) {
        result.push_back(it->second);
    }
    return result;
}

Status Engine::closeAll()
{
    std::lock_guard<std::mutex> guard(mutex_);

    Status firstError = Status::Ok;
    for (std::map<std::string, std::unique_ptr<SimulatorDevice>>::iterator it =
             open_.begin();
         it != open_.end();
         ++it) {
        const Status status = it->second->close();
        if (status != Status::Ok && firstError == Status::Ok) {
            firstError = status;
        }
    }
    open_.clear();
    return firstError;
}

} // namespace blockchronicle
