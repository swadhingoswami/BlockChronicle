// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_CONFIG_H
#define BLOCKCHRONICLE_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "blockchronicle/types.h"

namespace blockchronicle {

// Configuration subset that is actually implemented.
//
// Deliberately does not include retention limits yet: `collectGarbage` takes its
// policy as an argument, and exposing a config key that nothing reads would be
// worse than not having it. Retention configuration arrives with the service
// loop that calls the collector on a schedule.
struct Config {
    std::string rootDirectory = ".blockchronicle";
    std::size_t maxConsumers = 10;
    bool journalMarks = true;
};

// Minimal INI subset:
//
//   # comment
//   [storage]
//   root = /var/lib/blockchronicle
//
//   [backup]
//   max_consumers = 10
//
//   [simulator]
//   journal_marks = true
//
// Unknown sections, unknown keys and malformed values are errors with a message,
// never silently ignored: a typo in a retention setting must not be discovered
// during a restore.
Status parseConfig(const std::string& path, Config& out, std::string& error);

Status validateConfig(const Config& config, std::string& error);

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_CONFIG_H
