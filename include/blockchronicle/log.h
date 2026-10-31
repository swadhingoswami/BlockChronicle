// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_LOG_H
#define BLOCKCHRONICLE_LOG_H

#include <cstdint>
#include <string>

namespace blockchronicle {

enum class LogLevel : std::uint8_t {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

const char* toString(LogLevel level);

// Structured, line-oriented logging. One event per line as
//
//   ts=<epoch microseconds> level=info component=engine event=device_open device=x recovery=clean
//
// so that it can be parsed without a logging framework and without guessing at
// prose. No dynamic allocation is required by the interface; implementations may
// allocate.
class Logger {
public:
    virtual ~Logger() = default;
    virtual void log(LogLevel level,
                     const std::string& component,
                     const std::string& message) = 0;
};

class NullLogger final : public Logger {
public:
    void log(LogLevel, const std::string&, const std::string&) override {}
};

class StderrLogger final : public Logger {
public:
    explicit StderrLogger(LogLevel minimum = LogLevel::Info);

    void log(LogLevel level,
             const std::string& component,
             const std::string& message) override;

    void setMinimumLevel(LogLevel level) { minimum_ = level; }

private:
    LogLevel minimum_;
};

// Exposed for tests: formats one line without emitting it.
std::string formatLogLine(LogLevel level,
                          const std::string& component,
                          const std::string& message,
                          std::uint64_t timestampMicros);

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_LOG_H
