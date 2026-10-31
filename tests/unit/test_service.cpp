// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.
//
// Phase 8: configuration and structured logging.

#include <cstdio>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "blockchronicle/config.h"
#include "blockchronicle/log.h"
#include "support/status_printer.h"
#include "support/temp_dir.h"

namespace {

using namespace blockchronicle;

std::string writeConfig(const TempDir& directory,
                        const std::string& name,
                        const std::string& contents)
{
    const std::string path = directory.path() + "/" + name;
    std::ofstream output(path);
    output << contents;
    output.close();
    return path;
}

TEST(Config, ParsesEverySupportedKey)
{
    TempDir directory;
    const std::string path = writeConfig(directory, "bc.conf",
        "# comment\n"
        "[storage]\n"
        "root = /var/lib/blockchronicle\n"
        "\n"
        "[backup]\n"
        "max_consumers = 25\n"
        "\n"
        "[simulator]\n"
        "journal_marks = false\n");

    Config config;
    std::string error;
    ASSERT_EQ(parseConfig(path, config, error), Status::Ok) << error;

    EXPECT_EQ(config.rootDirectory, "/var/lib/blockchronicle");
    EXPECT_EQ(config.maxConsumers, 25u);
    EXPECT_FALSE(config.journalMarks);
}

TEST(Config, RejectsUnknownKeyWithLineNumber)
{
    TempDir directory;
    const std::string path = writeConfig(directory, "bc.conf",
        "[backup]\n"
        "max_consumer = 5\n"); // typo

    Config config;
    std::string error;
    EXPECT_EQ(parseConfig(path, config, error), Status::InvalidArgument);
    EXPECT_NE(error.find("line 2"), std::string::npos) << error;
    EXPECT_NE(error.find("max_consumer"), std::string::npos) << error;
}

TEST(Config, RejectsUnknownSection)
{
    TempDir directory;
    const std::string path = writeConfig(directory, "bc.conf", "[retention]\nkeep = 10\n");

    Config config;
    std::string error;
    EXPECT_EQ(parseConfig(path, config, error), Status::InvalidArgument);
    EXPECT_NE(error.find("retention"), std::string::npos);
}

TEST(Config, RejectsMalformedValues)
{
    TempDir directory;

    Config config;
    std::string error;

    const std::string badNumber = writeConfig(directory, "a.conf",
        "[backup]\nmax_consumers = ten\n");
    EXPECT_EQ(parseConfig(badNumber, config, error), Status::InvalidArgument);

    const std::string badBool = writeConfig(directory, "b.conf",
        "[simulator]\njournal_marks = maybe\n");
    EXPECT_EQ(parseConfig(badBool, config, error), Status::InvalidArgument);

    const std::string noEquals = writeConfig(directory, "c.conf", "[backup]\nmax_consumers\n");
    EXPECT_EQ(parseConfig(noEquals, config, error), Status::InvalidArgument);
}

TEST(Config, MissingFileIsReportedSeparately)
{
    TempDir directory;
    Config config;
    std::string error;
    EXPECT_EQ(parseConfig(directory.path() + "/absent.conf", config, error),
              Status::NotFound);
}

TEST(Config, ValidatesRanges)
{
    Config config;
    std::string error;

    config.maxConsumers = 0;
    EXPECT_EQ(validateConfig(config, error), Status::InvalidArgument);

    config.maxConsumers = 5000;
    EXPECT_EQ(validateConfig(config, error), Status::InvalidArgument);

    config.maxConsumers = 10;
    config.rootDirectory = "nfs://server/share";
    EXPECT_EQ(validateConfig(config, error), Status::InvalidArgument);

    config.rootDirectory = "/var/lib/blockchronicle";
    EXPECT_EQ(validateConfig(config, error), Status::Ok);
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

TEST(Log, LineHasParseableFields)
{
    const std::string line =
        formatLogLine(LogLevel::Warn, "engine", "event=device_open device=d1", 1234567u);

    EXPECT_NE(line.find("ts=1234567"), std::string::npos);
    EXPECT_NE(line.find("level=warn"), std::string::npos);
    EXPECT_NE(line.find("component=engine"), std::string::npos);
    EXPECT_NE(line.find("event=device_open"), std::string::npos);
    EXPECT_EQ(line.find('\n'), std::string::npos) << "one event per line";
}

TEST(Log, LevelNamesAreStable)
{
    EXPECT_STREQ(toString(LogLevel::Debug), "debug");
    EXPECT_STREQ(toString(LogLevel::Info), "info");
    EXPECT_STREQ(toString(LogLevel::Warn), "warn");
    EXPECT_STREQ(toString(LogLevel::Error), "error");
}

TEST(Log, NullLoggerIsSilent)
{
    NullLogger logger;
    logger.log(LogLevel::Error, "any", "should not appear");
    SUCCEED();
}

} // namespace
