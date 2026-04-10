#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/io_reactor.hpp"
#include "async/timer.hpp"

#include <coroutine>
#include <utility>

namespace rain::net {

class ConnectAwaiter {
public:
    ConnectAwaiter(async::EpollReactor& reactor, i32 fd) : reactor_(reactor), fd_(fd) { }

    [[nodiscard]] auto await_ready() noexcept -> bool { return false; }

    auto await_suspend(std::coroutine_handle<> h) -> bool
    {
        auto reg = reactor_.register_write(fd_, h, nullptr, 0, &readiness_);
        if (!reg) {
            readiness_ = Err(std::move(reg).error());
            return false;
        }
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<Unit>
    {
        if (!readiness_) {
            return Err(std::move(readiness_).error());
        }

        i32 err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
            return Err(SystemError::from_errno());
        }
        if (err != 0) {
            return Err(SystemError { .code = std::make_error_code(static_cast<std::errc>(err)),
                                     .message = "connect failed",
                                     .location = std::source_location::current() });
        }
        return Ok();
    }

private:
    async::EpollReactor& reactor_;
    i32 fd_;
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "connect not completed",
                                                 .location = std::source_location::current() });
};

class ReadReadyAwaiter {
public:
    ReadReadyAwaiter(async::EpollReactor& reactor, i32 fd) : reactor_(reactor), fd_(fd) { }

    [[nodiscard]] auto await_ready() noexcept -> bool { return false; }

    auto await_suspend(std::coroutine_handle<> h) -> bool
    {
        auto reg = reactor_.register_read(fd_, h, nullptr, 0, &readiness_);
        if (!reg) {
            readiness_ = Err(std::move(reg).error());
            return false;
        }
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<Unit>
    {
        if (!readiness_) {
            return Err(std::move(readiness_).error());
        }
        return Ok();
    }

private:
    async::EpollReactor& reactor_;
    i32 fd_;
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "read readiness not completed",
                                                 .location = std::source_location::current() });
};

class WriteReadyAwaiter {
public:
    WriteReadyAwaiter(async::EpollReactor& reactor, i32 fd) : reactor_(reactor), fd_(fd) { }

    [[nodiscard]] auto await_ready() noexcept -> bool { return false; }

    auto await_suspend(std::coroutine_handle<> h) -> bool
    {
        auto reg = reactor_.register_write(fd_, h, nullptr, 0, &readiness_);
        if (!reg) {
            readiness_ = Err(std::move(reg).error());
            return false;
        }
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<Unit>
    {
        if (!readiness_) {
            return Err(std::move(readiness_).error());
        }
        return Ok();
    }

private:
    async::EpollReactor& reactor_;
    i32 fd_;
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "write readiness not completed",
                                                 .location = std::source_location::current() });
};

class TimedReadyAwaiter {
public:
    enum class Direction : u8 { Read, Write };

    TimedReadyAwaiter(async::EpollReactor& reactor,
                      async::HierarchicalTimerWheel& timer,
                      i32 fd,
                      Direction direction,
                      async::Duration timeout)
        : reactor_(reactor), timer_(timer), fd_(fd), direction_(direction), timeout_(timeout)
    { }

    [[nodiscard]] auto await_ready() const noexcept -> bool { return false; }

    auto await_suspend(std::coroutine_handle<> h) -> bool
    {
        if (timeout_ <= async::Duration::zero()) {
            state_ = async::WaitState::TimedOut;
            return false;
        }

        timer_id_ = timer_.schedule(timeout_, h, &state_, async::WaitState::TimedOut);

        auto reg = (direction_ == Direction::Read)
                       ? reactor_.register_read(fd_, h, nullptr, 0, &readiness_, &state_, async::WaitState::Ready)
                       : reactor_.register_write(fd_, h, nullptr, 0, &readiness_, &state_, async::WaitState::Ready);

        if (!reg) {
            (void)timer_.cancel(timer_id_);
            readiness_ = Err(std::move(reg).error());
            return false;
        }

        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<Unit>
    {
        if (state_ == async::WaitState::TimedOut) {
            reactor_.deregister(fd_);
            return Err(SystemError { .code = std::make_error_code(std::errc::timed_out),
                                     .message = direction_ == Direction::Read ? "read timeout" : "write timeout",
                                     .location = std::source_location::current() });
        }

        (void)timer_.cancel(timer_id_);

        if (!readiness_) {
            return Err(std::move(readiness_).error());
        }

        return Ok();
    }

private:
    async::EpollReactor& reactor_;
    async::HierarchicalTimerWheel& timer_;
    i32 fd_;
    Direction direction_;
    async::Duration timeout_;
    async::TimerId timer_id_ = 0;
    async::WaitState state_ = async::WaitState::Pending;
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "timed readiness not completed",
                                                 .location = std::source_location::current() });
};

} // namespace rain::net
