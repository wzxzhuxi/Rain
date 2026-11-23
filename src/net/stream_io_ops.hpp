#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/concepts.hpp"
#include "async/event_loop.hpp"
#include "async/task.hpp"
#include "net/io_awaiters.hpp"

#include <cerrno>
#include <sys/sendfile.h>
#include <unistd.h>

namespace rain::net::detail {
// 统一重试环：立即试一次；EINTR 重试；EAGAIN 挂到 reactor 就绪后重试。
// read/write/sendfile 都以 >=0 为成功（read 返回 0 即 EOF）。
template<typename Op>
[[nodiscard]] inline auto retry_ready(async::EpollReactor& reactor, i32 fd, bool write_dir, Op op)
    -> async::Task<Result<usize>>
{
    while (true) {
        const auto bytes = op();
        if (bytes >= 0)
            co_return Ok(static_cast<usize>(bytes));
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            co_return Err(SystemError::from_errno());
        Result<Unit> waited = Ok();
        if (write_dir) {
            WriteReadyAwaiter ready(reactor, fd);
            waited = co_await ready;
        } else {
            ReadReadyAwaiter ready(reactor, fd);
            waited = co_await ready;
        }
        if (!waited)
            co_return Err(std::move(waited).error());
    }
}

// 带 deadline 的重试环；调用方保证 async::g_timer 已安装。
template<typename Op>
[[nodiscard]] inline auto retry_ready_until(async::EpollReactor& reactor, i32 fd, bool write_dir,
                                            async::TimePoint deadline, const char* timeout_msg, Op op)
    -> async::Task<Result<usize>>
{
    while (true) {
        const auto bytes = op();
        if (bytes >= 0)
            co_return Ok(static_cast<usize>(bytes));
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            co_return Err(SystemError::from_errno());
        const auto now = async::Clock::now();
        if (now >= deadline) {
            co_return Err(SystemError { .code = std::make_error_code(std::errc::timed_out),
                                        .message = timeout_msg,
                                        .location = std::source_location::current() });
        }

        const auto dir = write_dir ? TimedReadyAwaiter::Direction::Write : TimedReadyAwaiter::Direction::Read;
        TimedReadyAwaiter ready(reactor, *async::g_timer, fd, dir, deadline - now);
        auto waited = co_await ready;
        if (!waited)
            co_return Err(std::move(waited).error());
    }
}

[[nodiscard]] inline auto fail_task(SystemError err) -> async::Task<Result<usize>>
{
    co_return Err(std::move(err));
}

[[nodiscard]] inline auto read_some(async::EpollReactor& reactor, i32 fd, u8* buf, usize len)
    -> async::Task<Result<usize>>
{
    return retry_ready(reactor, fd, false, [fd, buf, len] { return ::read(fd, buf, len); });
}

[[nodiscard]] inline auto
read_some_with_timeout(async::EpollReactor& reactor, i32 fd, u8* buf, usize len, async::Duration timeout)
    -> async::Task<Result<usize>>
{
    if (timeout <= async::Duration::zero())
        return read_some(reactor, fd, buf, len);
    if (!async::g_timer)
        return fail_task(SystemError { .code = std::make_error_code(std::errc::operation_not_permitted),
                                       .message = "read_with_timeout requires event loop timer",
                                       .location = std::source_location::current() });
    return retry_ready_until(reactor, fd, false, async::Clock::now() + timeout, "read timeout",
        [fd, buf, len] { return ::read(fd, buf, len); });
}

[[nodiscard]] inline auto write_some(async::EpollReactor& reactor, i32 fd, const u8* buf, usize len)
    -> async::Task<Result<usize>>
{
    return retry_ready(reactor, fd, true, [fd, buf, len] { return ::write(fd, buf, len); });
}

[[nodiscard]] inline auto
write_some_with_timeout(async::EpollReactor& reactor, i32 fd, const u8* buf, usize len, async::Duration timeout)
    -> async::Task<Result<usize>>
{
    if (timeout <= async::Duration::zero())
        return write_some(reactor, fd, buf, len);
    if (!async::g_timer)
        return fail_task(SystemError { .code = std::make_error_code(std::errc::operation_not_permitted),
                                       .message = "write_with_timeout requires event loop timer",
                                       .location = std::source_location::current() });
    return retry_ready_until(reactor, fd, true, async::Clock::now() + timeout, "write timeout",
        [fd, buf, len] { return ::write(fd, buf, len); });
}

namespace detail_ {
inline auto broken_pipe(const char* msg) -> SystemError
{
    return { .code = std::make_error_code(std::errc::broken_pipe),
             .message = msg,
             .location = std::source_location::current() };
}
} // namespace detail_

[[nodiscard]] inline auto write_all(async::EpollReactor& reactor, i32 fd, const u8* buf, usize len)
    -> async::Task<Result<usize>>
{
    usize total = 0;
    while (total < len) {
        auto result = co_await write_some(reactor, fd, buf + total, len - total);
        if (!result)
            co_return Err(std::move(result).error());
        if (*result == 0)
            co_return Err(detail_::broken_pipe("write_all: connection closed"));
        total += *result;
    }
    co_return Ok(total);
}

[[nodiscard]] inline auto
write_all_with_timeout(async::EpollReactor& reactor, i32 fd, const u8* buf, usize len, async::Duration timeout)
    -> async::Task<Result<usize>>
{
    if (timeout <= async::Duration::zero())
        co_return co_await write_all(reactor, fd, buf, len);
    const auto deadline = async::Clock::now() + timeout;
    usize total = 0;
    while (total < len) {
        const auto now = async::Clock::now();
        if (now >= deadline) {
            co_return Err(SystemError { .code = std::make_error_code(std::errc::timed_out),
                                        .message = "write_all timeout",
                                        .location = std::source_location::current() });
        }
        auto result = co_await write_some_with_timeout(reactor, fd, buf + total, len - total, deadline - now);
        if (!result)
            co_return Err(std::move(result).error());
        if (*result == 0)
            co_return Err(detail_::broken_pipe("write_all_with_timeout: connection closed"));
        total += *result;
    }
    co_return Ok(total);
}

[[nodiscard]] inline auto send_file_some(async::EpollReactor& reactor, i32 fd, i32 in_fd, off_t& offset, usize count)
    -> async::Task<Result<usize>>
{
    return retry_ready(reactor, fd, true, [fd, in_fd, &offset, count] { return ::sendfile(fd, in_fd, &offset, count); });
}

[[nodiscard]] inline auto send_file_some_with_timeout(
    async::EpollReactor& reactor, i32 fd, i32 in_fd, off_t& offset, usize count, async::Duration timeout)
    -> async::Task<Result<usize>>
{
    if (timeout <= async::Duration::zero())
        return send_file_some(reactor, fd, in_fd, offset, count);
    if (!async::g_timer)
        return fail_task(SystemError { .code = std::make_error_code(std::errc::operation_not_permitted),
                                       .message = "send_file_with_timeout requires event loop timer",
                                       .location = std::source_location::current() });
    return retry_ready_until(reactor, fd, true, async::Clock::now() + timeout, "sendfile timeout",
        [fd, in_fd, &offset, count] { return ::sendfile(fd, in_fd, &offset, count); });
}

[[nodiscard]] inline auto send_file_all_with_timeout(
    async::EpollReactor& reactor, i32 fd, i32 in_fd, off_t offset, usize count, async::Duration timeout)
    -> async::Task<Result<usize>>
{
    const auto deadline = async::Clock::now() + timeout;
    usize total = 0;
    while (total < count) {
        const auto now = async::Clock::now();
        if (timeout > async::Duration::zero() && now >= deadline) {
            co_return Err(SystemError { .code = std::make_error_code(std::errc::timed_out),
                                        .message = "sendfile timeout",
                                        .location = std::source_location::current() });
        }
        auto result = timeout > async::Duration::zero()
                          ? co_await send_file_some_with_timeout(
                                reactor, fd, in_fd, offset, count - total, deadline - now)
                          : co_await send_file_some(reactor, fd, in_fd, offset, count - total);
        if (!result)
            co_return Err(std::move(result).error());
        if (*result == 0) {
            co_return Err(SystemError { .code = std::make_error_code(std::errc::io_error),
                                        .message = "sendfile reached EOF before requested byte count",
                                        .location = std::source_location::current() });
        }
        total += *result;
    }
    co_return Ok(total);
}
} // namespace rain::net::detail
