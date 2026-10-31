// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/crc32c.h"

namespace blockchronicle {
namespace {

struct Crc32cTable {
    std::uint32_t entries[256];

    Crc32cTable()
    {
        const std::uint32_t polynomial = 0x82F63B78u; // reflected 0x1EDC6F41
        for (std::uint32_t i = 0; i < 256u; ++i) {
            std::uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = ((crc & 1u) != 0u) ? ((crc >> 1u) ^ polynomial) : (crc >> 1u);
            }
            entries[i] = crc;
        }
    }
};

const Crc32cTable& table()
{
    static const Crc32cTable instance;
    return instance;
}

} // namespace

std::uint32_t crc32c(const void* data, std::size_t length, std::uint32_t seed)
{
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    const Crc32cTable& t = table();

    std::uint32_t crc = ~seed;
    for (std::size_t i = 0; i < length; ++i) {
        crc = t.entries[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8u);
    }
    return ~crc;
}

} // namespace blockchronicle
