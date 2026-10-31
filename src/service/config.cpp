// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace blockchronicle {
namespace {

std::string trim(const std::string& text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t'
                           || text[begin] == '\r')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'
                           || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool parseBool(const std::string& text, bool& out)
{
    if (text == "true" || text == "yes" || text == "1") {
        out = true;
        return true;
    }
    if (text == "false" || text == "no" || text == "0") {
        out = false;
        return true;
    }
    return false;
}

bool parseSize(const std::string& text, std::size_t& out)
{
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

} // namespace

Status parseConfig(const std::string& path, Config& out, std::string& error)
{
    error.clear();

    std::ifstream input(path);
    if (!input) {
        return Status::NotFound;
    }

    std::string section;
    std::string line;
    int lineNumber = 0;

    while (std::getline(input, line)) {
        ++lineNumber;

        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }
        const std::string trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }

        if (trimmed[0] == '[') {
            if (trimmed[trimmed.size() - 1] != ']') {
                error = "line " + std::to_string(lineNumber) + ": unterminated section";
                return Status::InvalidArgument;
            }
            section = trimmed.substr(1, trimmed.size() - 2);
            if (section != "storage" && section != "backup" && section != "simulator") {
                error = "line " + std::to_string(lineNumber) + ": unknown section [" + section
                    + "]";
                return Status::InvalidArgument;
            }
            continue;
        }

        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            error = "line " + std::to_string(lineNumber) + ": expected key = value";
            return Status::InvalidArgument;
        }

        const std::string key = trim(trimmed.substr(0, equals));
        const std::string value = trim(trimmed.substr(equals + 1));

        if (section == "storage" && key == "root") {
            if (value.empty()) {
                error = "line " + std::to_string(lineNumber) + ": empty root";
                return Status::InvalidArgument;
            }
            out.rootDirectory = value;
        } else if (section == "backup" && key == "max_consumers") {
            if (!parseSize(value, out.maxConsumers)) {
                error = "line " + std::to_string(lineNumber) + ": max_consumers must be a number";
                return Status::InvalidArgument;
            }
        } else if (section == "simulator" && key == "journal_marks") {
            if (!parseBool(value, out.journalMarks)) {
                error = "line " + std::to_string(lineNumber) + ": journal_marks must be true or false";
                return Status::InvalidArgument;
            }
        } else {
            error = "line " + std::to_string(lineNumber) + ": unknown key '" + key
                + "' in section [" + section + "]";
            return Status::InvalidArgument;
        }
    }

    return validateConfig(out, error);
}

Status validateConfig(const Config& config, std::string& error)
{
    error.clear();

    if (config.rootDirectory.empty()) {
        error = "root directory must not be empty";
        return Status::InvalidArgument;
    }
    if (config.rootDirectory.find("://") != std::string::npos) {
        error = "root directory must be a local path";
        return Status::InvalidArgument;
    }
    if (config.maxConsumers == 0u) {
        error = "max_consumers must be at least 1";
        return Status::InvalidArgument;
    }
    if (config.maxConsumers > 1000u) {
        error = "max_consumers is unreasonably large (limit 1000)";
        return Status::InvalidArgument;
    }
    return Status::Ok;
}

} // namespace blockchronicle
