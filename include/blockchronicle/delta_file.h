// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_DELTA_FILE_H
#define BLOCKCHRONICLE_DELTA_FILE_H

#include <string>

#include "blockchronicle/bitmap.h"
#include "blockchronicle/identity.h"
#include "blockchronicle/types.h"

namespace blockchronicle {

// One file per sealed generation: history/gen-<generation>.delta.
//
// Encoded as an explicit header plus the bitmap words in little-endian order.
// The header carries identity, granularity, region count, generation and a
// payload checksum; a file that does not match the current device is rejected
// rather than reinterpreted.

std::string deltaFileName(Generation generation);

Status deltaWrite(const std::string& path,
                  const MetadataIdentity& identity,
                  Generation generation,
                  const DirtyBitmap& delta);

Status deltaRead(const std::string& path,
                 const MetadataIdentity& identity,
                 const Geometry& geometry,
                 Generation& generation,
                 DirtyBitmap& out);

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_DELTA_FILE_H
