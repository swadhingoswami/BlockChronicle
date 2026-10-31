// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_IDENTITY_H
#define BLOCKCHRONICLE_IDENTITY_H

#include <array>
#include <cstdint>
#include <string>

namespace blockchronicle {

// Identity fields carried by every persisted object.
//
//   deviceUuid    stable identity of the block device
//   metadataUuid  random per metadata directory; detects a copied directory
//   deviceEpoch   bumped on every re-baseline; any cursor from an older epoch
//                 is meaningless
//
// A mismatch on any of them is an identity failure, never something to repair.
struct MetadataIdentity {
    std::array<std::uint8_t, 16> deviceUuid{};
    std::array<std::uint8_t, 16> metadataUuid{};
    std::uint64_t deviceEpoch = 1;

    // Deterministic identity for tests. Never use in production.
    static MetadataIdentity fromSeed(std::uint64_t seed);

    std::string deviceUuidHex() const;
    std::string metadataUuidHex() const;
};

bool operator==(const MetadataIdentity& a, const MetadataIdentity& b);
bool operator!=(const MetadataIdentity& a, const MetadataIdentity& b);

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_IDENTITY_H
