// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Internal word-level helpers shared by DirtyBitmap and ActiveBitmap. Kept in
// one place so that tail masking cannot diverge between the two implementations:
// a divergence here would mean a seal silently truncates or invents regions.

#ifndef BLOCKCHRONICLE_SRC_CORE_WORD_MATH_H
#define BLOCKCHRONICLE_SRC_CORE_WORD_MATH_H

#include <cstddef>
#include <cstdint>
#include <limits>

#include "blockchronicle/types.h"

namespace blockchronicle {
namespace internal {

inline std::size_t wordCountFor(std::uint32_t regionCount)
{
    return (static_cast<std::size_t>(regionCount) + (kBitsPerWord - 1u)) / kBitsPerWord;
}

// Bits at or above regionCount must always be zero.
inline Word tailMaskFor(std::uint32_t regionCount)
{
    const std::uint32_t usedBits = regionCount % kBitsPerWord;
    if (usedBits == 0u) {
        return std::numeric_limits<Word>::max();
    }
    return (Word{1} << usedBits) - Word{1};
}

// Bits [firstBit, lastBit], both inclusive and both < 64.
inline Word maskInclusive(std::uint32_t firstBit, std::uint32_t lastBit)
{
    const Word low = (firstBit == 0u)
        ? std::numeric_limits<Word>::max()
        : (std::numeric_limits<Word>::max() << firstBit);
    const Word high = (lastBit + 1u >= kBitsPerWord)
        ? std::numeric_limits<Word>::max()
        : ((Word{1} << (lastBit + 1u)) - Word{1});
    return low & high;
}

// Undefined for value == 0; every caller checks first.
inline std::uint32_t countTrailingZeros(Word value)
{
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<std::uint32_t>(__builtin_ctzll(value));
#else
    std::uint32_t count = 0;
    while ((value & Word{1}) == 0u) {
        value >>= 1u;
        ++count;
    }
    return count;
#endif
}

inline std::uint32_t popcountOfWord(Word value)
{
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<std::uint32_t>(__builtin_popcountll(value));
#else
    std::uint32_t count = 0;
    while (value != 0u) {
        value &= (value - Word{1});
        ++count;
    }
    return count;
#endif
}

} // namespace internal
} // namespace blockchronicle

#endif // BLOCKCHRONICLE_SRC_CORE_WORD_MATH_H
