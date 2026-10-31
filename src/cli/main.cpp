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

#include "blockchronicle/config.h"
#include "blockchronicle/engine.h"
#include "blockchronicle/log.h"
#include "blockchronicle/simulator.h"
#include "blockchronicle/verify.h"

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
    "  consumer remove <consumer-id> --device <device-id>\n"
    "  snapshot save --device <id> --generation <g> --label <name>\n"
    "  snapshot list --device <id>\n"
    "  snapshot delete --device <id> --label <name>\n"
    "  backup start --device <id> --consumer <id>\n"
    "  backup commit --device <id> --consumer <id> --backup-id <n> --boundary <gen>\n"
    "  bitmap compare --device <id> --from <gen> --to <gen>\n"
    "  checkpoint --device <id>\n"
    "  gc --device <id> [--keep-generations <n>]\n"
    "  verify --device <id>\n"
    "  status --device <id>\n"
    "\n"
    "Global options:\n"
    "  --config <file>           INI configuration (root, max_consumers, journal_marks)\n"
    "  --verbose                 structured logs on stderr\n";

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
    Status status = engine.device(deviceId, device);
    if (status != Status::Ok) {
        return fail("consumer list", status);
    }

    std::vector<ConsumerStatus> consumers;
    status = device->listConsumers(consumers);
    if (status != Status::Ok) {
        return fail("consumer list", status);
    }

    if (consumers.empty()) {
        std::cout << "no consumers\n";
        return 0;
    }

    std::cout << "consumer\tcursor\tlast-backup\tstate\n";
    for (std::size_t i = 0; i < consumers.size(); ++i) {
        const char* state = consumers[i].fullBackupRequired ? "full-required"
            : (consumers[i].sessionOpen ? "session-open" : "incremental");
        std::cout << consumers[i].consumerId << "\t" << consumers[i].cursor << "\t"
                  << consumers[i].lastBackupId << "\t" << state << "\n";
    }
    return 0;
}

int commandConsumerRemove(Engine& engine, const std::vector<std::string>& args)
{
    const std::string consumerId = positionalAt(args, 2);
    const std::string deviceId = valueOf(args, "--device");
    if (consumerId.empty() || deviceId.empty()) {
        std::cerr << "usage: consumer remove <consumer-id> --device <device-id>\n";
        return 1;
    }

    SimulatorDevice* device = nullptr;
    Status status = engine.device(deviceId, device);
    if (status != Status::Ok) {
        return fail("consumer remove", status);
    }
    status = device->removeConsumer(consumerId);
    if (status != Status::Ok) {
        return fail("consumer remove", status);
    }
    std::cout << "removed consumer " << consumerId << "\n";
    return 0;
}

int commandSnapshot(Engine& engine, const std::vector<std::string>& args)
{
    const std::string subcommand = positionalAt(args, 1);
    const std::string deviceId = valueOf(args, "--device");

    SimulatorDevice* device = nullptr;
    Status status = engine.device(deviceId, device);
    if (status != Status::Ok) {
        return fail("snapshot", status);
    }

    if (subcommand == "save") {
        const std::string generation = valueOf(args, "--generation");
        const std::string label = valueOf(args, "--label");
        if (generation.empty() || label.empty()) {
            std::cerr << "usage: snapshot save --device <id> --generation <g> "
                         "--label <name>\n";
            return 1;
        }
        status = device->saveSnapshot(std::strtoull(generation.c_str(), nullptr, 10),
                                      label);
        if (status != Status::Ok) {
            return fail("snapshot save", status);
        }
        std::cout << "saved snapshot " << label << "\n";
        return 0;
    }

    if (subcommand == "list") {
        std::vector<std::string> labels;
        status = device->listSnapshots(labels);
        if (status != Status::Ok) {
            return fail("snapshot list", status);
        }
        if (labels.empty()) {
            std::cout << "no snapshots\n";
            return 0;
        }
        for (std::size_t i = 0; i < labels.size(); ++i) {
            std::cout << labels[i] << "\n";
        }
        return 0;
    }

    if (subcommand == "delete") {
        const std::string label = valueOf(args, "--label");
        if (label.empty()) {
            std::cerr << "usage: snapshot delete --device <id> --label <name>\n";
            return 1;
        }
        status = device->deleteSnapshot(label);
        if (status != Status::Ok) {
            return fail("snapshot delete", status);
        }
        std::cout << "deleted snapshot " << label << "\n";
        return 0;
    }

    std::cerr << "usage: snapshot <save|list|delete> --device <id>\n";
    return 1;
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

int commandGc(Engine& engine, const std::vector<std::string>& args)
{
    const std::string deviceId = valueOf(args, "--device");
    const std::string keepText = valueOf(args, "--keep-generations");
    const std::uint64_t keep =
        keepText.empty() ? 0u : std::strtoull(keepText.c_str(), nullptr, 10);

    // No --device means every registered device, which is what the maintenance
    // timer unit calls.
    if (deviceId.empty()) {
        const std::vector<DeviceRecord> all = engine.records();
        std::uint64_t dropped = 0;
        for (std::size_t i = 0; i < all.size(); ++i) {
            SimulatorDevice* each = nullptr;
            Status status = engine.device(all[i].deviceId, each);
            if (status != Status::Ok) {
                return fail("gc", status);
            }
            GcResult result;
            status = each->collectGarbage(keep, result);
            if (status != Status::Ok) {
                return fail("gc", status);
            }
            dropped += result.droppedGenerations;
        }
        std::cout << "devices: " << all.size() << "\n";
        std::cout << "dropped-generations: " << dropped << "\n";
        return 0;
    }

    SimulatorDevice* device = nullptr;
    Status status = engine.device(deviceId, device);
    if (status != Status::Ok) {
        return fail("gc", status);
    }
    (void)device;

    // The collector lives on PersistentDevice, reached through the simulator.
    GcResult result;
    status = device->collectGarbage(keep, result);
    if (status != Status::Ok) {
        return fail("gc", status);
    }

    std::cout << "dropped-generations: " << result.droppedGenerations << "\n";
    std::cout << "dropped-through: " << result.droppedThrough << "\n";
    std::cout << "invalidated: " << result.invalidated.size() << "\n";
    for (std::size_t i = 0; i < result.invalidated.size(); ++i) {
        std::cout << "  " << result.invalidated[i] << "\n";
    }
    return 0;
}

int commandVerify(Engine& engine, const std::vector<std::string>& args)
{
    const std::string deviceId = valueOf(args, "--device");
    if (deviceId.empty()) {
        std::cerr << "usage: verify --device <id>\n";
        return 1;
    }

    const std::vector<DeviceRecord> records = engine.records();
    const DeviceRecord* found = nullptr;
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (records[i].deviceId == deviceId) {
            found = &records[i];
            break;
        }
    }
    if (found == nullptr) {
        return fail("verify", Status::NotFound);
    }

    VerifyReport report;
    const Status status = verifyDevice(found->metadataDirectory,
                                       Engine::identityOf(deviceId), report);
    if (status != Status::Ok) {
        return fail("verify", status);
    }

    std::cout << "superblock: " << (report.superblockValid ? "ok" : "bad") << "\n";
    std::cout << "identity: " << (report.identityValid ? "ok" : "bad") << "\n";
    std::cout << "history: " << (report.historyComplete ? "complete" : "with-gap") << "\n";
    std::cout << "journal: " << (report.journalClean ? "clean" : "corrupt") << "\n";
    std::cout << "torn-tail: " << (report.tornTail ? "yes" : "no") << "\n";
    std::cout << "clean-shutdown: " << (report.uncleanShutdown ? "no" : "yes") << "\n";
    std::cout << "generations-retained: " << report.generationsRetained << "\n";
    std::cout << "journal-records: " << report.journalRecords << "\n";
    std::cout << "consumers: " << report.consumers << "\n";
    if (!report.firstError.empty()) {
        std::cout << "first-error: " << report.firstError << "\n";
    }
    std::cout << "result: " << (report.healthy() ? "healthy" : "degraded") << "\n";

    return report.healthy() ? 0 : 2;
}

int commandCheckpoint(Engine& engine, const std::vector<std::string>& args)
{
    const std::string deviceId = valueOf(args, "--device");

    if (deviceId.empty()) {
        const std::vector<DeviceRecord> all = engine.records();
        for (std::size_t i = 0; i < all.size(); ++i) {
            SimulatorDevice* each = nullptr;
            const Status status = engine.device(all[i].deviceId, each);
            if (status != Status::Ok) {
                return fail("checkpoint", status);
            }
            const Status checkpointStatus = each->checkpoint();
            if (checkpointStatus != Status::Ok) {
                return fail("checkpoint", checkpointStatus);
            }
        }
        std::cout << "checkpointed " << all.size() << " devices\n";
        return 0;
    }

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
    std::string configPath;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string current = argv[i];
        if (current == "--root") {
            if (i + 1 < argc) {
                root = argv[++i];
            }
            continue;
        }
        if (current == "--config") {
            if (i + 1 < argc) {
                configPath = argv[++i];
            }
            continue;
        }
        if (current == "--verbose") {
            verbose = true;
            continue;
        }
        args.push_back(current);
    }

    if (args.empty() || args[0] == "--help" || args[0] == "help" || args[0] == "-h") {
        std::cout << kUsage;
        return args.empty() ? 1 : 0;
    }

    Config config;
    if (!configPath.empty()) {
        std::string error;
        const Status configStatus = parseConfig(configPath, config, error);
        if (configStatus != Status::Ok && configStatus != Status::NotFound) {
            std::cerr << "error: config: " << error << "\n";
            return 1;
        }
        if (configStatus == Status::NotFound) {
            std::cerr << "error: config: no such file: " << configPath << "\n";
            return 1;
        }
    }

    if (root.empty()) {
        const char* fromEnvironment = std::getenv("BLOCKCHRONICLE_ROOT");
        if (fromEnvironment != nullptr) {
            root = fromEnvironment;
        }
    }
    if (!root.empty()) {
        config.rootDirectory = root; // the command line wins over the file
    }

    // Quiet by default: a CLI command that succeeds should not narrate. Warnings
    // and errors still reach stderr; --verbose adds per-device detail.
    StderrLogger logger;
    logger.setMinimumLevel(verbose ? LogLevel::Debug : LogLevel::Warn);

    Engine engine(config.rootDirectory);
    engine.setLogger(&logger);
    DeviceOptions options;
    options.maxConsumers = config.maxConsumers;
    options.journalMarks = config.journalMarks;
    engine.setDeviceOptions(options);

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
    } else if (command == "snapshot") {
        result = commandSnapshot(engine, args);
    } else if (command == "bitmap" && subcommand == "compare") {
        result = commandBitmapCompare(engine, args);
    } else if (command == "status") {
        result = commandStatus(engine, args);
    } else if (command == "gc") {
        result = commandGc(engine, args);
    } else if (command == "verify") {
        result = commandVerify(engine, args);
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
