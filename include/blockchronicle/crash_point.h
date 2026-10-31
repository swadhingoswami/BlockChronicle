// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_CRASH_POINT_H
#define BLOCKCHRONICLE_CRASH_POINT_H

namespace blockchronicle {

// Named points where a test can terminate the process to simulate a crash.
//
// A throw from hit() is a faithful simulation because no destructor flushes or
// marks anything clean: durability is only ever established by an explicit
// write in the code path, never by cleanup.
class CrashPoint {
public:
    virtual ~CrashPoint() = default;
    virtual void hit(const char* name) = 0;
};

class NoCrashPoint final : public CrashPoint {
public:
    void hit(const char*) override {}
};

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_CRASH_POINT_H
