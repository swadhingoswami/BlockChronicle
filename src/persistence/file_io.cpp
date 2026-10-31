// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "file_io.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace blockchronicle {
namespace internal {
namespace {

Status errnoStatus()
{
    return Status::IoError;
}

} // namespace

Status readWholeFile(const std::string& path, std::vector<std::uint8_t>& out)
{
    out.clear();

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return (errno == ENOENT) ? Status::NotFound : Status::IoError;
    }

    std::uint8_t buffer[64 * 1024];
    for (;;) {
        const ssize_t readBytes = ::read(fd, buffer, sizeof(buffer));
        if (readBytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return errnoStatus();
        }
        if (readBytes == 0) {
            break;
        }
        out.insert(out.end(), buffer, buffer + readBytes);
    }

    ::close(fd);
    return Status::Ok;
}

Status syncFile(int fd)
{
    while (::fsync(fd) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return errnoStatus();
    }
    return Status::Ok;
}

Status fsyncDirectory(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return errnoStatus();
    }

    Status status = Status::Ok;
    if (::fsync(fd) != 0 && errno != EINVAL) {
        // Some filesystems refuse fsync on a directory descriptor. That is not
        // a reason to fail the operation; it only weakens the rename guarantee.
        status = errnoStatus();
    }

    ::close(fd);
    return status;
}

Status writeFileAtomic(const std::string& path, const std::vector<std::uint8_t>& data)
{
    const std::string temporary = path + ".tmp";

    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return errnoStatus();
    }

    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            ::unlink(temporary.c_str());
            return errnoStatus();
        }
        offset += static_cast<std::size_t>(written);
    }

    if (syncFile(fd) != Status::Ok) {
        ::close(fd);
        ::unlink(temporary.c_str());
        return errnoStatus();
    }
    ::close(fd);

    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return errnoStatus();
    }

    const std::size_t slash = path.find_last_of('/');
    const std::string directory = (slash == std::string::npos) ? "." : path.substr(0, slash);
    return fsyncDirectory(directory);
}

Status appendFile(const std::string& path, const std::vector<std::uint8_t>& data)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        return errnoStatus();
    }

    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return errnoStatus();
        }
        offset += static_cast<std::size_t>(written);
    }

    const Status status = syncFile(fd);
    ::close(fd);
    return status;
}

Status ensureDirectory(const std::string& path)
{
    if (path.empty()) {
        return Status::InvalidArgument;
    }

    // mkdir -p semantics: a caller asking for <root>/devices/<id>/meta should not
    // have to create each level itself.
    std::string current;
    std::size_t index = 0;
    if (path[0] == '/') {
        current = "/";
        index = 1;
    }

    for (;;) {
        const std::size_t next = path.find('/', index);
        const std::string component = (next == std::string::npos)
            ? path.substr(index)
            : path.substr(index, next - index);

        if (!component.empty()) {
            if (current.empty()) {
                current = component;
            } else if (current == "/") {
                current += component;
            } else {
                current += '/';
                current += component;
            }
            if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) {
                return Status::IoError;
            }
        }

        if (next == std::string::npos) {
            break;
        }
        index = next + 1u;
    }

    struct stat info;
    if (::stat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        return Status::IoError;
    }
    return Status::Ok;
}

Status removeFile(const std::string& path)
{
    if (::unlink(path.c_str()) == 0 || errno == ENOENT) {
        return Status::Ok;
    }
    return errnoStatus();
}

Status truncateFile(const std::string& path, std::uint64_t size)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return errnoStatus();
    }
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ::close(fd);
        return errnoStatus();
    }
    const Status status = syncFile(fd);
    ::close(fd);
    return status;
}

Status listDirectory(const std::string& path, std::vector<std::string>& names)
{
    names.clear();

    DIR* directory = ::opendir(path.c_str());
    if (directory == nullptr) {
        return (errno == ENOENT) ? Status::NotFound : errnoStatus();
    }

    struct dirent* entry = nullptr;
    while ((entry = ::readdir(directory)) != nullptr) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        names.push_back(name);
    }

    ::closedir(directory);
    return Status::Ok;
}

Status lockDirectory(const std::string& directory, int& fd)
{
    fd = -1;
    const std::string path = directory + "/LOCK";
    const int handle = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (handle < 0) {
        return Status::IoError;
    }
    if (::flock(handle, LOCK_EX | LOCK_NB) != 0) {
        const bool busy = (errno == EWOULDBLOCK || errno == EAGAIN);
        ::close(handle);
        return busy ? Status::InvalidState : Status::IoError;
    }
    fd = handle;
    return Status::Ok;
}

bool fileExists(const std::string& path)
{
    struct stat info;
    return ::stat(path.c_str(), &info) == 0;
}

} // namespace internal
} // namespace blockchronicle
