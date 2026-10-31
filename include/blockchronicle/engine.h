// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_ENGINE_H
#define BLOCKCHRONICLE_ENGINE_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "blockchronicle/crash_point.h"
#include "blockchronicle/identity.h"
#include "blockchronicle/simulator.h"
#include "blockchronicle/types.h"

namespace blockchronicle {

struct DeviceRecord {
    std::string deviceId;
    std::string metadataDirectory;
    std::string dataPath;
    std::uint64_t capacityBytes = 0;
};

// Multi-device registry. Each device is an independent SimulatorDevice with its
// own metadata directory, lock, history and consumers; one device's failure or
// slowness must not affect another.
//
// The registry file is a cache. The authoritative state is each device's
// manifest.bin, so a lost or stale registry can be rebuilt by scanning the
// devices directory. Identity is derived from the device id (FNV-1a) so that a
// registration is reproducible across runs; a real backend reads the device's
// WWID instead.
class Engine {
public:
    explicit Engine(std::string rootDirectory);

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Status initialise();

    Status registerSimulatorDevice(const std::string& deviceId,
                                   const std::string& dataPath,
                                   std::uint64_t capacityBytes);

    // Explicit administrative removal. Deletes history for the device, which is
    // the only operation in the system allowed to do so.
    Status removeDevice(const std::string& deviceId);

    Status device(const std::string& deviceId, SimulatorDevice*& out);

    std::vector<DeviceRecord> records() const;

    Status closeAll();

private:
    Status loadRegistry();
    Status saveRegistry();
    std::string registryPath() const;
    std::string deviceDirectory(const std::string& deviceId) const;
    static MetadataIdentity identityFor(const std::string& deviceId);

    std::string root_;
    mutable std::mutex mutex_;
    std::map<std::string, DeviceRecord> records_;
    std::map<std::string, std::unique_ptr<SimulatorDevice>> open_;
    NoCrashPoint crash_;
};

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_ENGINE_H
