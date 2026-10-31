// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// blockchronicle CLI. One process per command, so backup sessions are not kept
// in memory: `backup start` reports the boundary and ranges, and
// `backup commit` supplies the boundary back. The in-process library API keeps
// sessions and is the safer interface for a real backup application.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "blockchronicle/engine.h"
#include "blockchronicle/simulator.h"

namespace {

using namespace blockchronicle;

const char* kUsage =
    "blockchronicle - Linux block-level change tracking (simulator backend)\n"
    "\n"
    "Global:\n"
    "  --root <dir>              metadata root (default $BLOCKCHRONICLE_ROOT or ./.blockchronicle)\n"
    "\n"
    "Commands:\n"
    "  device list\n"
    "  device register <device-id> --data <file> --capacity <bytes>\n"
    "  device remove <device-id>\n"
    "  consumer add <consumer-id> --device <device-id>\n"
    "  consumer list --device <device-id>\n"
    "  backup start --device <id> --consumer <id>\n"
    "  backup commit --device <id> --consumer <id> --backup-id <n> --boundary <gen>\n"
    "  bitmap compare --device <id> --from <gen> --to <gen>\n"
    "  checkpoint --device <id>\n"
    "  status --device <id>\n";

std::string valueOf(const std::vector<std::string>& args, const char* name)
{
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            return args[i + 1];
        }
    }
    return std::string();
}

// Returns the nth non-option token. Options always consume the following token,
// so `--device x` never counts as a positional.
std::string positionalAt(const std::vector<std::string>& args, std::size_t index)
{
    std::size_t seen = 0;
    bool skipNext = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (skipNext) {
            skipNext = false;
            continue;
        }
        if (args[i].size() > 1 && args[i][0] == '-') {
            skipNext = true;
            continue;
        }
        if (seen == index) {
            return args[i];
        }
        ++seen;
    }
    return std::string();
}

int fail(const char* operation, Status status)
{
    std::cerr << "error: " << operation << ": " << toString(status) << "\n";
    return 1;
}

int commandDeviceList(Engine& engine)
{
    const std::vector<DeviceRecord> records = engine.records();
    if (records.empty()) {
        std::cout << "no devices registered\n";
        return 0;
    }
    for (std::size_t i = 0; i < records.size(); ++i) {
        std::cout << records[i].deviceId << "\t" << records[i].capacityBytes << "\t"
                  << records[i].dataPath << "\n";
    }
    return 0;
}

int commandDeviceRegister(Engine& engine, const std::vector<std::string>& args)
{
    const std::string deviceId = positionalAt(args, 2);
    const std::string dataPath = valueOf(args, "--data");
    const std::string capacityText = valueOf(args, "--capacity");

    if (deviceId.empty() || dataPath.empty() || capacityText.empty()) {
        std::cerr << "usage: device register <device-id> --data <file> --capacity <bytes>\n";
        return 1;
    }

    const std::uint64_t capacity = std::strtoull(capacityText.c_str(), nullptr, 10);
    const Status status = engine.registerSimulatorDevice(deviceId, dataPath, capacity);
    if (status != Status::Ok) {
        return fail("device register", status);
    }
    std::cout << "registered " << deviceId << "\n";
    return 0;
}

int commandDeviceRemove(Engine& engine, const std::vector<std::string>& args)
{
    const std::string deviceId = positionalAt(args, 2);
    if (deviceId.empty()) {
        return fail("device remove", Status::InvalidArgument);
    }
    const Status status = engine.removeDevice(deviceId);
    if (status != Status::Ok) {
        return fail("device remove", status);
    }
    std::cout << "removed " << deviceId << " (history deleted)\n";
    return 0;
}

int commandConsumerAdd(Engine& engine, const std::vector<std::string>& args)
{
    const std::string consumerId = positionalAt(args, 2);
    const std::string deviceId = valueOf(args, "--device");
    if (consumerId.empty() || deviceId.empty()) {
        std::cerr << "usage: consumer add <consumer-id> --device <device-id>\n";
        return 1;
    }

    SimulatorDevice* device = nullptr;
    Status status = engine.device(deviceId, device);
    if (status != Status::Ok) {
        return fail("consumer add", status);
    }
    status = device->addConsumer(consumerId);
    if (status != Status::Ok) {
        return fail("consumer add", status);
    }
    std::cout << "added consumer " << consumerId << " to " << deviceId << "\n";
    std::cout << "a full backup is required before incremental backups\n";
    return 0;
}

int commandConsumerList(Engine& engine, const std::vector<std::string>& args)
{
    const std::string deviceId = valueOf(args, "--device");
    SimulatorDevice* device = nullptr;
    const Status status = engine.device(deviceId, device);
    if (status != Status::Ok) {
        return fail("consumer list", status);
    }
    (void)device;
    std::cerr << "note: listing consumers requires the in-process API; "
                 "use 'status --device <id>'\n";
    return 0;
}

int commandBackupStart(Engine& engine, const std::vector<std::string>& args)
{
    const std::string deviceId = valueOf(args, "--device");
    const std::string consumerId = valueOf(args, "--consumer");
    if (deviceId.empty() || consumerId.empty()) {
        std::cerr << "usage: backup start --device <id> --consumer <id>\n";
        return 1;
    }

    SimulatorDevice* device = nullptr;
    Status status = engine.device(deviceId, device);
    if (status != Status::Ok) {
        return fail("backup start", status);
    }

    BackupSession session;
    SnapshotHandle snapshot;
    status = device->beginBackup(consumerId, session, snapshot);
    if (status != Status::Ok) {
        return fail("backup start", status);
    }

    std::cout << "boundary: " << session.boundary << "\n";
    std::cout << "cursor: " << session.cursorAtStart << "\n";

    if (session.kind == BackupKind::Full) {
        std::cout << "kind: full\n";
        std::cout << "action: copy the whole device\n";
    } else {
        std::vector<DirtyRange> ranges;
        status = device->getDirtyRanges(session, 1u << 20, ranges);
        if (status != Status::Ok) {
            device->abortBackup(session);
            device->releaseSnapshot(snapshot);
            return fail("backup start", status);
        }
        std::cout << "kind: incremental\n";
        std::cout << "ranges: " << ranges.size() << "\n";
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            std::cout << "  " << ranges[i].offsetBytes << " " << ranges[i].lengthBytes
                      << "\n";
            total += ranges[i].lengthBytes;
        }
        std::cout << "dirty-bytes: " << total << "\n";
    }

    // The CLI does not transfer data, so the session and its snapshot end here.
    device->abortBackup(session);
    device->releaseSnapshot(snapshot);
    return 0;
}

int commandBackupCommit(Engine& engine, const std::vector<std::string>& args)
{
    const std::string deviceId = valueOf(args, "--device");
    const std::string consumerId = valueOf(args, "--consumer");
    const std::string backupText = valueOf(args, "--backup-id");
    const std::string boundaryText = valueOf(args, "--boundary");

    if (deviceId.empty() || consumerId.empty() || backupText.empty()
        || boundaryText.empty()) {
        std::cerr << "usage: backup commit --device <id> --consumer <id> "
                     "--backup-id <n> --boundary <gen>\n";
        return 1;
    }

    SimulatorDevice* device = nullptr;
    Status status = engine.device(deviceId, device);
    if (status != Status::Ok) {
        return fail("backup commit", status);
    }

    status = device->commitBackupAt(consumerId,
                                    std::strtoull(backupText.c_str(), nullptr, 10),
                                    std::strtoull(boundaryText.c_str(), nullptr, 10));
    if (status != Status::Ok) {
        return fail("backup commit", status);
    }
    std::cout << "committed " << consumerId << " at generation " << boundaryText << "\n";
    return 0;
}

int commandBitmapCompare(Engine& engine, const std::vector<std::string>& args)
{
    const std::string deviceId = valueOf(args, "--device");
    const std::string fromText = valueOf(args, "--from");
    const std::string toText = valueOf(args, "--to");
    if (deviceId.empty() || fromText.empty() || toText.empty()) {
        std::cerr << "usage: bitmap compare --device <id> --from <gen> --to <gen>\n";
        return 1;
    }

    SimulatorDevice* device = nullptr;
    Status status = engine.device(deviceId, device);
    if (status != Status::Ok) {
        return fail("bitmap compare", status);
    }

    DirtyBitmap result(device->geometry());
    status = device->compareGenerations(std::strtoull(fromText.c_str(), nullptr, 10),
                                        std::strtoull(toText.c_str(), nullptr, 10),
                                        result);
    if (status != Status::Ok) {
        return fail("bitmap compare", status);
    }

    std::vector<DirtyRange> ranges;
    status = result.toRanges(1u << 20, ranges);
    if (status != Status::Ok) {
        return fail("bitmap compare", status);
    }

    std::cout << "dirty-regions: " << result.countDirty() << "\n";
    std::cout << "dirty-bytes: " << result.dirtyBytes() << "\n";
    std::cout << "ranges: " << ranges.size() << "\n";
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        std::cout << "  " << ranges[i].offsetBytes << " " << ranges[i].lengthBytes << "\n";
    }
    return 0;
}

int commandStatus(Engine& engine, const std::vector<std::string>& args)
{
    const std::string deviceId = valueOf(args, "--device");
    SimulatorDevice* device = nullptr;
    Status status = engine.device(deviceId, device);
    if (status != Status::Ok) {
        return fail("status", status);
    }

    std::cout << "device: " << deviceId << "\n";
    std::cout << "capacity: " << device->capacityBytes() << "\n";
    std::cout << "granularity: " << device->geometry().granularityBytes() << "\n";
    std::cout << "active-generation: " << device->activeGeneration() << "\n";
    std::cout << "last-sealed: " << device->lastSealedGeneration() << "\n";

    const char* state = "created";
    switch (device->recoveryState()) {
    case RecoveryState::Created: state = "created"; break;
    case RecoveryState::Clean: state = "clean"; break;
    case RecoveryState::Replayed: state = "replayed"; break;
    case RecoveryState::Invalidated: state = "invalidated"; break;
    }
    std::cout << "recovery: " << state << "\n";
    return 0;
}

int commandCheckpoint(Engine& engine, const std::vector<std::string>& args)
{
    const std::string deviceId = valueOf(args, "--device");
    SimulatorDevice* device = nullptr;
    Status status = engine.device(deviceId, device);
    if (status != Status::Ok) {
        return fail("checkpoint", status);
    }
    status = device->checkpoint();
    if (status != Status::Ok) {
        return fail("checkpoint", status);
    }
    std::cout << "checkpointed " << deviceId << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> args;
    std::string root;

    for (int i = 1; i < argc; ++i) {
        const std::string current = argv[i];
        if (current == "--root") {
            if (i + 1 < argc) {
                root = argv[++i];
            }
            continue;
        }
        args.push_back(current);
    }

    if (args.empty() || args[0] == "--help" || args[0] == "help" || args[0] == "-h") {
        std::cout << kUsage;
        return args.empty() ? 1 : 0;
    }

    if (root.empty()) {
        const char* fromEnvironment = std::getenv("BLOCKCHRONICLE_ROOT");
        root = (fromEnvironment != nullptr) ? fromEnvironment : ".blockchronicle";
    }

    Engine engine(root);
    Status status = engine.initialise();
    if (status != Status::Ok) {
        return fail("initialise", status);
    }

    const std::string command = args[0];
    const std::string subcommand = positionalAt(args, 1);

    int result = 1;
    if (command == "device") {
        if (subcommand == "list") {
            result = commandDeviceList(engine);
        } else if (subcommand == "register") {
            result = commandDeviceRegister(engine, args);
        } else if (subcommand == "remove") {
            result = commandDeviceRemove(engine, args);
        } else {
            std::cout << kUsage;
        }
    } else if (command == "consumer") {
        if (subcommand == "add") {
            result = commandConsumerAdd(engine, args);
        } else if (subcommand == "list") {
            result = commandConsumerList(engine, args);
        } else {
            std::cout << kUsage;
        }
    } else if (command == "backup") {
        if (subcommand == "start") {
            result = commandBackupStart(engine, args);
        } else if (subcommand == "commit") {
            result = commandBackupCommit(engine, args);
        } else {
            std::cout << kUsage;
        }
    } else if (command == "bitmap" && subcommand == "compare") {
        result = commandBitmapCompare(engine, args);
    } else if (command == "status") {
        result = commandStatus(engine, args);
    } else if (command == "checkpoint") {
        result = commandCheckpoint(engine, args);
    } else {
        std::cout << kUsage;
    }

    const Status closeStatus = engine.closeAll();
    if (result == 0 && closeStatus != Status::Ok) {
        return fail("shutdown", closeStatus);
    }
    return result;
}
