#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/event_loop.hpp"
#include "async/task.hpp"
#include "fs/metadata.hpp"
#include "runtime/runtime.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <source_location>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace rain::fs::detail {

[[nodiscard]] inline auto make_error(std::errc code,
                                     String message,
                                     std::source_location loc = std::source_location::current()) -> SystemError
{
    return SystemError { .code = std::make_error_code(code), .message = std::move(message), .location = loc };
}

template<typename F>
using BlockingResult = std::invoke_result_t<std::decay_t<F>&>;

template<typename F>
    requires std::invocable<std::decay_t<F>&>
[[nodiscard]] auto spawn_blocking_result(F&& fn) -> async::Task<BlockingResult<F>>
{
    using ResultT = BlockingResult<F>;

    if (!async::g_reactor) {
        co_return Err(make_error(std::errc::operation_not_permitted,
                                 "fs operation requires a running EventLoop"));
    }
    if (!runtime::g_thread_pool) {
        co_return Err(make_error(std::errc::operation_not_permitted,
                                 "fs operation requires Runtime blocking pool"));
    }

    auto work = std::decay_t<F>(std::forward<F>(fn));
    auto nested = co_await runtime::spawn_blocking(*async::g_reactor, *runtime::g_thread_pool,
                                                   [work = std::move(work)]() mutable -> ResultT {
                                                       return work();
                                                   });
    if (!nested) {
        co_return Err(std::move(nested).error());
    }

    co_return std::move(*nested);
}

class OwnedFd {
public:
    OwnedFd(const OwnedFd&) = delete;
    auto operator=(const OwnedFd&) -> OwnedFd& = delete;

    explicit OwnedFd(i32 fd = -1) : fd_(fd) { }

    OwnedFd(OwnedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) { }

    auto operator=(OwnedFd&& other) noexcept -> OwnedFd&
    {
        if (this != &other) {
            (void)close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~OwnedFd() { (void)close(); }

    [[nodiscard]] auto fd() const noexcept -> i32 { return fd_; }
    [[nodiscard]] auto valid() const noexcept -> bool { return fd_ >= 0; }

    [[nodiscard]] auto release() noexcept -> i32 { return std::exchange(fd_, -1); }

    [[nodiscard]] auto close() -> Result<Unit>
    {
        if (fd_ < 0) {
            return Ok();
        }

        const i32 fd = std::exchange(fd_, -1);
        if (::close(fd) < 0) {
            return Err(SystemError::from_errno());
        }
        return Ok();
    }

private:
    i32 fd_ = -1;
};

[[nodiscard]] inline auto checked_offset(u64 offset) -> Result<off_t>
{
    constexpr auto max_off = static_cast<u64>(std::numeric_limits<off_t>::max());
    if (offset > max_off) {
        return Err(make_error(std::errc::invalid_argument, "file offset is too large"));
    }
    return Ok(static_cast<off_t>(offset));
}

[[nodiscard]] inline auto checked_offset_add(u64 offset, usize delta) -> Result<off_t>
{
    constexpr auto max_off = static_cast<u64>(std::numeric_limits<off_t>::max());
    const auto delta64 = static_cast<u64>(delta);
    if (offset > max_off || delta64 > max_off - offset) {
        return Err(make_error(std::errc::invalid_argument, "file offset is too large"));
    }
    return Ok(static_cast<off_t>(offset + delta64));
}

[[nodiscard]] inline auto open_fd(const String& path, i32 flags, mode_t mode = 0644) -> Result<OwnedFd>
{
    const i32 fd = (flags & O_CREAT) ? ::open(path.c_str(), flags | O_CLOEXEC, mode)
                                     : ::open(path.c_str(), flags | O_CLOEXEC);
    if (fd < 0) {
        return Err(SystemError::from_errno());
    }
    return Ok(OwnedFd(fd));
}

[[nodiscard]] inline auto write_all_fd(i32 fd, const u8* data, usize len) -> Result<Unit>
{
    usize total = 0;
    while (total < len) {
        const auto written = ::write(fd, data + total, len - total);
        if (written > 0) {
            total += static_cast<usize>(written);
            continue;
        }
        if (written == 0) {
            return Err(make_error(std::errc::io_error, "write returned zero"));
        }
        if (errno == EINTR) {
            continue;
        }
        return Err(SystemError::from_errno());
    }
    return Ok();
}

[[nodiscard]] inline auto read_file_sync(const String& path) -> Result<Bytes>
{
    auto fd_result = open_fd(path, O_RDONLY);
    if (!fd_result) {
        return Err(std::move(fd_result).error());
    }
    auto fd = std::move(*fd_result);

    Bytes data;
    struct stat st { };
    if (::fstat(fd.fd(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        data.reserve(static_cast<usize>(st.st_size));
    }

    std::array<u8, 8192> buf { };
    while (true) {
        const auto n = ::read(fd.fd(), buf.data(), buf.size());
        if (n > 0) {
            data.insert(data.end(), buf.begin(), buf.begin() + n);
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        return Err(SystemError::from_errno());
    }

    if (auto close_result = fd.close(); !close_result) {
        return Err(std::move(close_result).error());
    }
    return Ok(std::move(data));
}

[[nodiscard]] inline auto write_file_sync(const String& path, const u8* data, usize len, bool append) -> Result<Unit>
{
    const i32 flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    auto fd_result = open_fd(path, flags, 0644);
    if (!fd_result) {
        return Err(std::move(fd_result).error());
    }
    auto fd = std::move(*fd_result);

    if (auto write_result = write_all_fd(fd.fd(), data, len); !write_result) {
        return Err(std::move(write_result).error());
    }
    return fd.close();
}

[[nodiscard]] inline auto read_at_fd_sync(i32 fd, u64 offset, usize len) -> Result<Bytes>
{
    auto off = checked_offset(offset);
    if (!off) {
        return Err(std::move(off).error());
    }

    Bytes data(len);
    usize total = 0;
    while (total < len) {
        auto current = checked_offset_add(offset, total);
        if (!current) {
            return Err(std::move(current).error());
        }

        const auto n = ::pread(fd, data.data() + total, len - total, *current);
        if (n > 0) {
            total += static_cast<usize>(n);
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        return Err(SystemError::from_errno());
    }

    data.resize(total);
    return Ok(std::move(data));
}

[[nodiscard]] inline auto write_at_fd_sync(i32 fd, u64 offset, const u8* data, usize len) -> Result<usize>
{
    usize total = 0;
    while (total < len) {
        auto current = checked_offset_add(offset, total);
        if (!current) {
            return Err(std::move(current).error());
        }

        const auto n = ::pwrite(fd, data + total, len - total, *current);
        if (n > 0) {
            total += static_cast<usize>(n);
            continue;
        }
        if (n == 0) {
            return Err(make_error(std::errc::io_error, "pwrite returned zero"));
        }
        if (errno == EINTR) {
            continue;
        }
        return Err(SystemError::from_errno());
    }
    return Ok(total);
}

[[nodiscard]] inline auto metadata_fd_sync(i32 fd) -> Result<Metadata>
{
    struct stat st { };
    if (::fstat(fd, &st) < 0) {
        return Err(SystemError::from_errno());
    }
    return Ok(metadata_from_stat(st));
}

[[nodiscard]] inline auto metadata_path_sync(const String& path, bool follow_symlink) -> Result<Metadata>
{
    struct stat st { };
    const i32 rc = follow_symlink ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st);
    if (rc < 0) {
        return Err(SystemError::from_errno());
    }
    return Ok(metadata_from_stat(st));
}

[[nodiscard]] inline auto exists_sync(const String& path) -> Result<bool>
{
    if (::access(path.c_str(), F_OK) == 0) {
        return Ok(true);
    }
    if (errno == ENOENT) {
        return Ok(false);
    }
    return Err(SystemError::from_errno());
}

[[nodiscard]] inline auto remove_file_sync(const String& path) -> Result<Unit>
{
    if (::unlink(path.c_str()) < 0) {
        return Err(SystemError::from_errno());
    }
    return Ok();
}

} // namespace rain::fs::detail
