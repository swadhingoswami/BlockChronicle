// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_CRC32C_H
#define BLOCKCHRONICLE_CRC32C_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace blockchronicle {

// CRC32C (Castagnoli, polynomial 0x1EDC6F41, reflected form 0x82F63B78).
// Standard test vector: crc32c("123456789") == 0xE3069283.
std::uint32_t crc32c(const void* data, std::size_t length, std::uint32_t seed = 0);

inline std::uint32_t crc32c(const std::vector<std::uint8_t>& bytes)
{
    return crc32c(bytes.data(), bytes.size());
}

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_CRC32C_H
