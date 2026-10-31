// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/verify.h"

#include "blockchronicle/delta_file.h"
#include "blockchronicle/superblock.h"
#include "blockchronicle/wal.h"
#include "persistence/file_io.h"

namespace blockchronicle {
namespace {

class CountingHandler final : public WalHandler {
public:
    Status onRecord(const WalRecord&) override
    {
        ++records;
        return Status::Ok;
    }

    std::uint64_t records = 0;
};

} // namespace

Status verifyDevice(const std::string& metadataDirectory,
                    const MetadataIdentity& expectedIdentity,
                    VerifyReport& out)
{
    out = VerifyReport();

    const std::string manifest = metadataDirectory + "/manifest.bin";
    SuperblockState state;
    bool found = false;
    Status status = superblockRead(manifest, expectedIdentity, state, found);
    if (status != Status::Ok) {
        out.firstError = "superblock unreadable";
        return Status::Ok; // the report carries the finding
    }
    if (!found) {
        out.firstError = "manifest.bin missing";
        return Status::Ok;
    }
    out.superblockValid = true;

    // superblockRead already refuses foreign identities, so reaching this point
    // means the device and metadata UUIDs matched.
    out.identityValid = true;

    Geometry geometry;
    status = Geometry::make(state.capacityBytes, state.granularityBytes, geometry);
    if (status != Status::Ok) {
        out.firstError = "geometry invalid";
        return Status::Ok;
    }

    out.uncleanShutdown = !state.cleanShutdown;
    out.consumers = static_cast<std::uint64_t>(state.consumers.size());

    out.historyComplete = true;
    if (state.lastSealedGeneration >= state.oldestRetainedGeneration
        && state.lastSealedGeneration != 0u) {
        for (Generation generation = state.oldestRetainedGeneration;
             generation <= state.lastSealedGeneration;
             ++generation) {
            DirtyBitmap delta(geometry);
            Generation readGeneration = 0;
            const std::string path = metadataDirectory + "/history/"
                + deltaFileName(generation);
            const Status readStatus =
                deltaRead(path, expectedIdentity, geometry, readGeneration, delta);
            if (readStatus != Status::Ok || readGeneration != generation) {
                out.historyComplete = false;
                out.firstError = "history gap at generation "
                    + std::to_string(static_cast<unsigned long long>(generation));
                break;
            }
            ++out.generationsRetained;
        }
    }

    CountingHandler handler;
    const WalReplaySummary replay =
        replayWal(metadataDirectory + "/wal", state.walFirstSequence, handler);
    out.journalRecords = handler.records;
    out.tornTail = replay.tornTail;

    if (replay.status == Status::Ok) {
        out.journalClean = true;
    } else {
        out.journalClean = false;
        if (out.firstError.empty()) {
            out.firstError = "journal corrupt: ";
            out.firstError += toString(replay.status);
        }
    }

    return Status::Ok;
}

} // namespace blockchronicle
