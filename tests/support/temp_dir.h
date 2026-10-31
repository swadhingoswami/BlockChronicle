// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_TESTS_SUPPORT_TEMP_DIR_H
#define BLOCKCHRONICLE_TESTS_SUPPORT_TEMP_DIR_H

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

class TempDir {
public:
    TempDir()
    {
        static std::atomic<int> counter{0};
        const int index = counter.fetch_add(1);
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "blockchronicle-test-%d-%d",
                      static_cast<int>(::getpid()), index);
        path_ = (std::filesystem::temp_directory_path() / buffer).string();

        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
    }

    ~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

#endif // BLOCKCHRONICLE_TESTS_SUPPORT_TEMP_DIR_H
