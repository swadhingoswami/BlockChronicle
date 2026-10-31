// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Test-only helper: makes EXPECT_EQ print a readable status name.

#ifndef BLOCKCHRONICLE_TESTS_SUPPORT_STATUS_PRINTER_H
#define BLOCKCHRONICLE_TESTS_SUPPORT_STATUS_PRINTER_H

#include <ostream>

#include "blockchronicle/types.h"

namespace blockchronicle {

inline std::ostream& operator<<(std::ostream& os, Status status)
{
    return os << toString(status);
}

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_TESTS_SUPPORT_STATUS_PRINTER_H
