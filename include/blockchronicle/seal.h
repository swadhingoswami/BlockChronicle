// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_SEAL_H
#define BLOCKCHRONICLE_SEAL_H

#include <memory>

#include "blockchronicle/bitmap.h"
#include "blockchronicle/types.h"

namespace blockchronicle {

// A sealed generation's delta. Immutable by type: no caller can modify history.
using SealedDelta = std::shared_ptr<const DirtyBitmap>;

// Persistence hook for the seal protocol.
//
// Call contract: invoked while the device's exclusive lock is held. It must make
// the delta durable before returning Ok. It must not call back into the same
// GenerationManager or HistoryStore (that would self-deadlock).
//
// Returning anything other than Ok aborts the seal completely: nothing is
// published, the active generation keeps its bits and keeps collecting writes.
class SealSink {
public:
    virtual ~SealSink() = default;
    virtual Status commitDelta(Generation generation, const DirtyBitmap& delta) = 0;
};

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_SEAL_H
