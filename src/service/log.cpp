// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/log.h"

#include <chrono>
#include <cstdio>

namespace blockchronicle {

const char* toString(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    }
    return "unknown";
}

std::string formatLogLine(LogLevel level,
                          const std::string& component,
                          const std::string& message,
                          std::uint64_t timestampMicros)
{
    std::string line;
    line.reserve(96u + component.size() + message.size());

    char timestamp[32];
    std::snprintf(timestamp, sizeof(timestamp), "%llu",
                  static_cast<unsigned long long>(timestampMicros));
    line += "ts=";
    line += timestamp;

    line += " level=";
    line += toString(level);

    line += " component=";
    line += component;

    // The message is already key=value pairs; it is appended verbatim so that a
    // caller can emit fields without the logger having to understand them.
    line += ' ';
    line += message;
    return line;
}

StderrLogger::StderrLogger(LogLevel minimum)
    : minimum_(minimum)
{
}

void StderrLogger::log(LogLevel level,
                       const std::string& component,
                       const std::string& message)
{
    if (static_cast<int>(level) < static_cast<int>(minimum_)) {
        return;
    }

    using Clock = std::chrono::system_clock;
    const std::uint64_t micros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch())
            .count());

    const std::string line = formatLogLine(level, component, message, micros);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
}

} // namespace blockchronicle
