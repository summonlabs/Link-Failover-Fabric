// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "detail/fsutil.hpp"

#include <cerrno>
#include <cstdio>
#include <string>
#include <system_error>

#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>
#endif

namespace lff::detail {
namespace {

Status io_failure(const char* what, const std::filesystem::path& path, int code) {
    Status status = Status::failure(Outcome::IoError, ReasonCode::InternalError, what);
    status.add(ReasonCode::InvalidArgument, path.filename().string());
    status.add(ReasonCode::SocketError, "errno=" + std::to_string(code));
    return status;
}

Status sync_stream(std::FILE* stream, const std::filesystem::path& path) {
    if (std::fflush(stream) != 0) {
        return io_failure("flush failed", path, errno);
    }
#if defined(_WIN32)
    const int descriptor = ::_fileno(stream);
    if (descriptor < 0) {
        return io_failure("fileno failed", path, errno);
    }
    if (::_commit(descriptor) != 0) {
        return io_failure("commit failed", path, errno);
    }
#else
    const int descriptor = ::fileno(stream);
    if (descriptor < 0) {
        return io_failure("fileno failed", path, errno);
    }
    if (::fsync(descriptor) != 0) {
        return io_failure("fsync failed", path, errno);
    }
#endif
    return Status::success();
}

}  // namespace

Status ensure_directory(const std::filesystem::path& path) {
    if (path.empty()) {
        return Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                               "state directory must not be empty");
    }
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        if (error) {
            return Status::failure(Outcome::IoError, ReasonCode::InternalError,
                                   "cannot stat state directory");
        }
        if (!std::filesystem::is_directory(path, error)) {
            return Status::failure(Outcome::Invalid, ReasonCode::InvalidArgument,
                                   "state path exists and is not a directory");
        }
        return Status::success();
    }
    std::filesystem::create_directories(path, error);
    if (error) {
        return Status::failure(Outcome::IoError, ReasonCode::InternalError,
                               "cannot create state directory");
    }
    return Status::success();
}

bool path_exists(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    return exists && !error;
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return Result<std::uint64_t>::failure(Outcome::NotFound, ReasonCode::UnknownLink,
                                              "file does not exist");
    }
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        return Result<std::uint64_t>::failure(Outcome::IoError, ReasonCode::InternalError,
                                              "cannot read file size");
    }
    return Result<std::uint64_t>::success(static_cast<std::uint64_t>(size));
}

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                            std::uint64_t max_bytes) {
    const Result<std::uint64_t> size = lff::detail::file_size(path);
    if (!size.ok()) {
        return Result<std::vector<std::uint8_t>>::failure(size.status());
    }
    if (size.value() > max_bytes) {
        Status status = Status::failure(Outcome::LimitExceeded, ReasonCode::LimitExceeded,
                                        "file exceeds the configured maximum");
        status.add(ReasonCode::InvalidArgument, std::to_string(size.value()));
        return Result<std::vector<std::uint8_t>>::failure(status);
    }
    std::FILE* stream = nullptr;
#if defined(_WIN32)
    if (::fopen_s(&stream, path.string().c_str(), "rb") != 0) {
        stream = nullptr;
    }
#else
    stream = std::fopen(path.string().c_str(), "rb");
#endif
    if (stream == nullptr) {
        return Result<std::vector<std::uint8_t>>::failure(
            io_failure("open failed", path, errno));
    }
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size.value()));
    std::size_t read_total = 0;
    while (read_total < buffer.size()) {
        const std::size_t read = std::fread(buffer.data() + read_total, 1,
                                            buffer.size() - read_total, stream);
        if (read == 0) {
            break;
        }
        read_total += read;
    }
    const bool short_read = read_total != buffer.size();
    std::fclose(stream);
    if (short_read) {
        return Result<std::vector<std::uint8_t>>::failure(
            io_failure("short read", path, errno));
    }
    return Result<std::vector<std::uint8_t>>::success(std::move(buffer));
}

Result<std::vector<std::uint8_t>> read_prefix(const std::filesystem::path& path,
                                              std::uint64_t max_bytes) {
    std::FILE* stream = nullptr;
#if defined(_WIN32)
    if (::fopen_s(&stream, path.string().c_str(), "rb") != 0) {
        stream = nullptr;
    }
#else
    stream = std::fopen(path.string().c_str(), "rb");
#endif
    if (stream == nullptr) {
        return Result<std::vector<std::uint8_t>>::failure(
            io_failure("open failed", path, errno));
    }
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(max_bytes));
    const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), stream);
    const bool failed = std::ferror(stream) != 0;
    std::fclose(stream);
    if (failed) {
        return Result<std::vector<std::uint8_t>>::failure(
            io_failure("read failed", path, errno));
    }
    buffer.resize(read);
    return Result<std::vector<std::uint8_t>>::success(std::move(buffer));
}

Status write_file_atomic(const std::filesystem::path& path,
                         const std::vector<std::uint8_t>& data) {
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    std::FILE* stream = nullptr;
#if defined(_WIN32)
    if (::fopen_s(&stream, temporary.string().c_str(), "wb") != 0) {
        stream = nullptr;
    }
#else
    stream = std::fopen(temporary.string().c_str(), "wb");
#endif
    if (stream == nullptr) {
        return io_failure("cannot create temporary file", temporary, errno);
    }
    if (!data.empty()) {
        const std::size_t written = std::fwrite(data.data(), 1, data.size(), stream);
        if (written != data.size()) {
            std::fclose(stream);
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return io_failure("short write", temporary, errno);
        }
    }
    Status synced = sync_stream(stream, temporary);
    std::fclose(stream);
    if (!synced.ok()) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return synced;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return io_failure("atomic replace failed", path, error.value());
    }
    return Status::success();
}

Status truncate_file(const std::filesystem::path& path, std::uint64_t size) {
    std::FILE* stream = nullptr;
#if defined(_WIN32)
    if (::fopen_s(&stream, path.string().c_str(), "r+b") != 0) {
        stream = nullptr;
    }
#else
    stream = std::fopen(path.string().c_str(), "r+b");
#endif
    if (stream == nullptr) {
        return io_failure("cannot open for truncation", path, errno);
    }
#if defined(_WIN32)
    const int descriptor = ::_fileno(stream);
    const errno_t result = ::_chsize_s(descriptor, static_cast<__int64>(size));
    const int code = static_cast<int>(result);
#else
    const int descriptor = ::fileno(stream);
    const int result = ::ftruncate(descriptor, static_cast<off_t>(size));
    const int code = result;
#endif
    if (result != 0) {
        std::fclose(stream);
        return io_failure("truncate failed", path, code);
    }
    Status synced = sync_stream(stream, path);
    std::fclose(stream);
    return synced;
}

Status remove_file(const std::filesystem::path& path) noexcept {
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) {
        return Status::failure(Outcome::IoError, ReasonCode::InternalError, "remove failed");
    }
    return Status::success();
}

}  // namespace lff::detail
