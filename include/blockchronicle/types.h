// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_TYPES_H
#define BLOCKCHRONICLE_TYPES_H

#include <cstdint>

namespace blockchronicle {

using Word = std::uint64_t;
using Generation = std::uint64_t;
using BackupId = std::uint64_t;

constexpr std::uint32_t kDefaultGranularityBytes = 64u * 1024u;
constexpr std::uint32_t kBitsPerWord = 64u;

// Every fallible operation returns Status. No exceptions escape the library.
enum class Status : std::uint8_t {
    Ok = 0,
    InvalidArgument,
    OutOfRange,
    InvalidState,
    NotFound,
    Corrupt,
    Unsupported,
    IoError,
};

inline const char* toString(Status status)
{
    switch (status) {
    case Status::Ok: return "Ok";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::OutOfRange: return "OutOfRange";
    case Status::InvalidState: return "InvalidState";
    case Status::NotFound: return "NotFound";
    case Status::Corrupt: return "Corrupt";
    case Status::Unsupported: return "Unsupported";
    case Status::IoError: return "IoError";
    }
    return "Unknown";
}

// A byte range that is dirty. Always aligned to the tracking granularity except
// for the length of the final, partially tracked region at the end of a device.
struct DirtyRange {
    std::uint64_t offsetBytes = 0;
    std::uint64_t lengthBytes = 0;
};

// Inclusive span of region indices.
struct RegionSpan {
    std::uint32_t firstRegion = 0;
    std::uint32_t lastRegion = 0;
};

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_TYPES_H
