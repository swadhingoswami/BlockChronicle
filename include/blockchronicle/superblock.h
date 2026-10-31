// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_SUPERBLOCK_H
#define BLOCKCHRONICLE_SUPERBLOCK_H

#include <cstdint>
#include <string>
#include <vector>

#include "blockchronicle/identity.h"
#include "blockchronicle/types.h"

namespace blockchronicle {

// Consumer state as persisted at checkpoint time. Pending bitmaps are not
// stored: they are rebuilt from the deltas on recovery.
struct PersistedConsumer {
    std::string consumerId;
    BackupId lastBackupId = 0;
    Generation cursor = 0;
    Generation cursorEpoch = 0;
    bool fullBackupRequired = true;
};

// The complete checkpointed state of one device.
struct SuperblockState {
    std::uint64_t sequence = 0;

    Generation activeGeneration = 1;
    Generation lastSealedGeneration = 0;
    Generation oldestRetainedGeneration = 1;
    std::uint64_t walFirstSequence = 1;

    // Device geometry, so that open() needs nothing but the directory and the
    // identity.
    std::uint64_t capacityBytes = 0;
    std::uint32_t granularityBytes = 0;
    std::uint32_t regionCount = 0;
    std::uint64_t deviceEpoch = 1;

    bool trackingValid = true;
    bool cleanShutdown = false;
    bool durableBeforeAck = true;

    std::vector<PersistedConsumer> consumers;
};

// manifest.bin holds two 4 KiB slots. Writers alternate and the highest valid
// sequence wins, so a torn write to one slot cannot destroy the state.
constexpr std::uint32_t kSuperblockSlotSize = 4096u;

// Reads the highest-sequence valid slot. `found` is false when the file does not
// exist; the status is Ok in that case. Identity mismatches return Corrupt.
Status superblockRead(const std::string& path,
                      const MetadataIdentity& identity,
                      SuperblockState& out,
                      bool& found);

// Writes the next sequence into the slot that does not hold the current state.
// The caller's sequence field is updated.
Status superblockWrite(const std::string& path,
                       const MetadataIdentity& identity,
                       SuperblockState& state);

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_SUPERBLOCK_H
