// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Internal POSIX file helpers. Every durable write in the project goes through
// these, so the fsync discipline is in one place.

#ifndef BLOCKCHRONICLE_SRC_PERSISTENCE_FILE_IO_H
#define BLOCKCHRONICLE_SRC_PERSISTENCE_FILE_IO_H

#include <cstdint>
#include <string>
#include <vector>

#include "blockchronicle/types.h"

namespace blockchronicle {
namespace internal {

Status readWholeFile(const std::string& path, std::vector<std::uint8_t>& out);

// Writes to <path>.tmp, fsyncs it, renames over <path>, then fsyncs the
// directory. A crash leaves either the old file or the new one, never a mix.
Status writeFileAtomic(const std::string& path, const std::vector<std::uint8_t>& data);

Status appendFile(const std::string& path, const std::vector<std::uint8_t>& data);

Status syncFile(int fd);

Status fsyncDirectory(const std::string& path);

Status ensureDirectory(const std::string& path);

Status removeFile(const std::string& path);

Status truncateFile(const std::string& path, std::uint64_t size);

Status listDirectory(const std::string& path, std::vector<std::string>& names);

bool fileExists(const std::string& path);

// Takes an exclusive non-blocking flock on <directory>/LOCK. Returns
// Status::InvalidState when another process holds it. The caller owns the fd.
Status lockDirectory(const std::string& directory, int& fd);

} // namespace internal
} // namespace blockchronicle

#endif // BLOCKCHRONICLE_SRC_PERSISTENCE_FILE_IO_H
