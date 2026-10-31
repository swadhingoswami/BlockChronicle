// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/identity.h"

namespace blockchronicle {
namespace {

std::uint64_t nextRandom(std::uint64_t& state)
{
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state;
}

void fillUuid(std::array<std::uint8_t, 16>& uuid, std::uint64_t& state)
{
    for (std::size_t i = 0; i < uuid.size(); i += 8u) {
        const std::uint64_t value = nextRandom(state);
        for (std::size_t b = 0; b < 8u; ++b) {
            uuid[i + b] = static_cast<std::uint8_t>((value >> (8u * b)) & 0xFFu);
        }
    }
}

std::string toHex(const std::array<std::uint8_t, 16>& uuid)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(32);
    for (std::size_t i = 0; i < uuid.size(); ++i) {
        text.push_back(kDigits[(uuid[i] >> 4u) & 0xFu]);
        text.push_back(kDigits[uuid[i] & 0xFu]);
    }
    return text;
}

} // namespace

MetadataIdentity MetadataIdentity::fromSeed(std::uint64_t seed)
{
    MetadataIdentity identity;
    std::uint64_t state = seed * 2862933555777941757ull + 3037000493ull;
    fillUuid(identity.deviceUuid, state);
    fillUuid(identity.metadataUuid, state);
    identity.deviceEpoch = 1;
    return identity;
}

std::string MetadataIdentity::deviceUuidHex() const
{
    return toHex(deviceUuid);
}

std::string MetadataIdentity::metadataUuidHex() const
{
    return toHex(metadataUuid);
}

bool operator==(const MetadataIdentity& a, const MetadataIdentity& b)
{
    return a.deviceUuid == b.deviceUuid && a.metadataUuid == b.metadataUuid
        && a.deviceEpoch == b.deviceEpoch;
}

bool operator!=(const MetadataIdentity& a, const MetadataIdentity& b)
{
    return !(a == b);
}

} // namespace blockchronicle
