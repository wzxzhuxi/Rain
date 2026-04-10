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

[[nodiscard]] inline auto read_some(async::EpollReactor& reactor, i32 fd, u8* buf, usize len)
    -> async::Task<Result<usize>>
{
    while (true) {
        const auto bytes = ::read(fd, buf, len);
        if (bytes > 0) {
            co_return Ok(static_cast<usize>(bytes));
        }
        if (bytes == 0) {
            co_return Ok(usize { 0 });
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ReadReadyAwaiter ready(reactor, fd);
            auto wait_result = co_await ready;
            if (!wait_result) {
                co_return Err(std::move(wait_result).error());
            }
            continue;
        }

        co_return Err(SystemError::from_errno());
    }
}

[[nodiscard]] inline auto
read_some_with_timeout(async::EpollReactor& reactor, i32 fd, u8* buf, usize len, async::Duration timeout)
    -> async::Task<Result<usize>>
{
    if (timeout <= async::Duration::zero()) {
        co_return co_await read_some(reactor, fd, buf, len);
    }

    if (!async::g_timer) {
        co_return Err(SystemError { .code = std::make_error_code(std::errc::operation_not_permitted),
                                    .message = "read_with_timeout requires event loop timer",
                                    .location = std::source_location::current() });
    }

    const auto deadline = async::Clock::now() + timeout;

    while (true) {
        const auto bytes = ::read(fd, buf, len);
        if (bytes > 0) {
            co_return Ok(static_cast<usize>(bytes));
        }
        if (bytes == 0) {
            co_return Ok(usize { 0 });
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            const auto now = async::Clock::now();
            if (now >= deadline) {
                co_return Err(SystemError { .code = std::make_error_code(std::errc::timed_out),
                                            .message = "read timeout",
                                            .location = std::source_location::current() });
            }

            TimedReadyAwaiter ready(reactor, *async::g_timer, fd, TimedReadyAwaiter::Direction::Read, deadline - now);
            auto wait_result = co_await ready;
            if (!wait_result) {
                co_return Err(std::move(wait_result).error());
            }
            continue;
        }

        co_return Err(SystemError::from_errno());
    }
}

[[nodiscard]] inline auto write_some(async::EpollReactor& reactor, i32 fd, const u8* buf, usize len)
    -> async::Task<Result<usize>>
{
    while (true) {
        const auto bytes = ::write(fd, buf, len);
        if (bytes >= 0) {
            co_return Ok(static_cast<usize>(bytes));
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            WriteReadyAwaiter ready(reactor, fd);
            auto wait_result = co_await ready;
            if (!wait_result) {
                co_return Err(std::move(wait_result).error());
            }
            continue;
        }

        co_return Err(SystemError::from_errno());
    }
}

[[nodiscard]] inline auto
write_some_with_timeout(async::EpollReactor& reactor, i32 fd, const u8* buf, usize len, async::Duration timeout)
    -> async::Task<Result<usize>>
{
    if (timeout <= async::Duration::zero()) {
        co_return co_await write_some(reactor, fd, buf, len);
    }

    if (!async::g_timer) {
        co_return Err(SystemError { .code = std::make_error_code(std::errc::operation_not_permitted),
                                    .message = "write_with_timeout requires event loop timer",
                                    .location = std::source_location::current() });
    }

    const auto deadline = async::Clock::now() + timeout;

    while (true) {
        const auto bytes = ::write(fd, buf, len);
        if (bytes >= 0) {
            co_return Ok(static_cast<usize>(bytes));
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            const auto now = async::Clock::now();
            if (now >= deadline) {
                co_return Err(SystemError { .code = std::make_error_code(std::errc::timed_out),
                                            .message = "write timeout",
                                            .location = std::source_location::current() });
            }

            TimedReadyAwaiter ready(reactor, *async::g_timer, fd, TimedReadyAwaiter::Direction::Write, deadline - now);
            auto wait_result = co_await ready;
            if (!wait_result) {
                co_return Err(std::move(wait_result).error());
            }
            continue;
        }

        co_return Err(SystemError::from_errno());
    }
}

[[nodiscard]] inline auto write_all(async::EpollReactor& reactor, i32 fd, const u8* buf, usize len)
    -> async::Task<Result<usize>>
{
    usize total = 0;
    while (total < len) {
        auto result = co_await write_some(reactor, fd, buf + total, len - total);
        if (!result) {
            co_return Err(std::move(result).error());
        }
        if (*result == 0) {
            co_return Err(SystemError { .code = std::make_error_code(std::errc::broken_pipe),
                                        .message = "write_all: connection closed",
                                        .location = std::source_location::current() });
        }
        total += *result;
    }
    co_return Ok(total);
}

[[nodiscard]] inline auto
write_all_with_timeout(async::EpollReactor& reactor, i32 fd, const u8* buf, usize len, async::Duration timeout)
    -> async::Task<Result<usize>>
{
    if (timeout <= async::Duration::zero()) {
        co_return co_await write_all(reactor, fd, buf, len);
    }

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
        if (!result) {
            co_return Err(std::move(result).error());
        }
        if (*result == 0) {
            co_return Err(SystemError { .code = std::make_error_code(std::errc::broken_pipe),
                                        .message = "write_all_with_timeout: connection closed",
                                        .location = std::source_location::current() });
        }
        total += *result;
    }
    co_return Ok(total);
}

[[nodiscard]] inline auto send_file_some(async::EpollReactor& reactor, i32 fd, i32 in_fd, off_t& offset, usize count)
    -> async::Task<Result<usize>>
{
    while (true) {
        const auto bytes = ::sendfile(fd, in_fd, &offset, count);
        if (bytes >= 0) {
            co_return Ok(static_cast<usize>(bytes));
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            WriteReadyAwaiter ready(reactor, fd);
            auto wait_result = co_await ready;
            if (!wait_result) {
                co_return Err(std::move(wait_result).error());
            }
            continue;
        }

        co_return Err(SystemError::from_errno());
    }
}

[[nodiscard]] inline auto send_file_some_with_timeout(
    async::EpollReactor& reactor, i32 fd, i32 in_fd, off_t& offset, usize count, async::Duration timeout)
    -> async::Task<Result<usize>>
{
    if (timeout <= async::Duration::zero()) {
        co_return co_await send_file_some(reactor, fd, in_fd, offset, count);
    }

    if (!async::g_timer) {
        co_return Err(SystemError { .code = std::make_error_code(std::errc::operation_not_permitted),
                                    .message = "send_file_with_timeout requires event loop timer",
                                    .location = std::source_location::current() });
    }

    const auto deadline = async::Clock::now() + timeout;

    while (true) {
        const auto bytes = ::sendfile(fd, in_fd, &offset, count);
        if (bytes >= 0) {
            co_return Ok(static_cast<usize>(bytes));
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            const auto now = async::Clock::now();
            if (now >= deadline) {
                co_return Err(SystemError { .code = std::make_error_code(std::errc::timed_out),
                                            .message = "sendfile timeout",
                                            .location = std::source_location::current() });
            }

            TimedReadyAwaiter ready(reactor, *async::g_timer, fd, TimedReadyAwaiter::Direction::Write, deadline - now);
            auto wait_result = co_await ready;
            if (!wait_result) {
                co_return Err(std::move(wait_result).error());
            }
            continue;
        }

        co_return Err(SystemError::from_errno());
    }
}

[[nodiscard]] inline auto send_file_all_with_timeout(
    async::EpollReactor& reactor, i32 fd, i32 in_fd, off_t offset, usize count, async::Duration timeout)
    -> async::Task<Result<usize>>
{
    usize total = 0;
    const auto deadline = async::Clock::now() + timeout;

    while (total < count) {
        if (timeout > async::Duration::zero()) {
            const auto now = async::Clock::now();
            if (now >= deadline) {
                co_return Err(SystemError { .code = std::make_error_code(std::errc::timed_out),
                                            .message = "sendfile timeout",
                                            .location = std::source_location::current() });
            }

            auto result
                = co_await send_file_some_with_timeout(reactor, fd, in_fd, offset, count - total, deadline - now);
            if (!result) {
                co_return Err(std::move(result).error());
            }
            if (*result == 0) {
                co_return Err(SystemError { .code = std::make_error_code(std::errc::io_error),
                                            .message = "sendfile reached EOF before requested byte count",
                                            .location = std::source_location::current() });
            }
            total += *result;
            continue;
        }

        auto result = co_await send_file_some(reactor, fd, in_fd, offset, count - total);
        if (!result) {
            co_return Err(std::move(result).error());
        }
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
