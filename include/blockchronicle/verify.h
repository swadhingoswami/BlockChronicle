// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_VERIFY_H
#define BLOCKCHRONICLE_VERIFY_H

#include <cstdint>
#include <string>

#include "blockchronicle/identity.h"
#include "blockchronicle/types.h"

namespace blockchronicle {

// Result of a read-only inspection of a device's metadata. Verification never
// repairs and never writes: it answers "what would recovery do with this
// directory, and why".
struct VerifyReport {
    bool superblockValid = false;
    bool identityValid = false;
    bool historyComplete = false;
    bool journalClean = false;
    bool uncleanShutdown = false;
    bool tornTail = false;

    std::uint64_t generationsRetained = 0;
    std::uint64_t journalRecords = 0;
    std::uint64_t consumers = 0;

    std::string firstError;

    bool healthy() const
    {
        return superblockValid && identityValid && historyComplete && journalClean
            && !uncleanShutdown;
    }
};

// Opens nothing, locks nothing, writes nothing. Safe to run against a metadata
// directory that a live service may be using (it can observe a torn journal
// tail and report it, which is not an error).
Status verifyDevice(const std::string& metadataDirectory,
                    const MetadataIdentity& expectedIdentity,
                    VerifyReport& out);

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_VERIFY_H
