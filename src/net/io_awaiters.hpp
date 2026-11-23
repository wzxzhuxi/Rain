#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/event_loop.hpp"
#include "async/io_reactor.hpp"
#include "async/timer.hpp"

#include <coroutine>
#include <functional>
#include <optional>
#include <stop_token>
#include <sys/socket.h>
#include <utility>

namespace rain::net {
namespace detail {
// 抢占式取消的共用收尾：owner 核上与 reactor.poll/timer.tick 串行执行，故无需原子。
// Pending→Cancelled、摘除 reactor 方向、调度唤醒；await_resume 见 Cancelled 即返回错误。
template<typename R>
inline auto cancel_io_wait(R& reactor, i32 fd, async::IoEvent dir, async::WaitState& state,
                           std::coroutine_handle<> handle) -> void
{
    if (state != async::WaitState::Pending)
        return;
    state = async::WaitState::Cancelled;
    (void)reactor.deregister(fd, dir);
    if (async::g_event_loop && handle)
        async::g_event_loop->notify(handle);
}

[[nodiscard]] inline auto io_cancelled_error() -> SystemError
{
    return SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                         .message = "io operation cancelled",
                         .location = std::source_location::current() };
}

// 读/写就绪等待器的公共实现；R 只需满足 IoReactorLike（测试可注入 SimReactor）。
template<typename R, bool kWrite>
class ReadyAwaiter {
public:
    ReadyAwaiter(R& reactor, i32 fd) : reactor_(reactor), fd_(fd) { }

    auto set_cancellation(std::stop_token token) -> void { token_ = std::move(token); }

    [[nodiscard]] auto await_ready() noexcept -> bool { return false; }

    auto await_suspend(std::coroutine_handle<> h) -> bool
    {
        handle_ = h;
        auto reg = kWrite ? reactor_.register_write(fd_, h, nullptr, 0, &readiness_, &state_, async::WaitState::Ready)
                          : reactor_.register_read(fd_, h, nullptr, 0, &readiness_, &state_, async::WaitState::Ready);
        if (!reg) {
            readiness_ = Err(std::move(reg).error());
            return false;
        }
        if (token_.stop_possible())
            cancel_cb_.emplace(token_, [this] { cancel_io_wait(reactor_, fd_, direction(), state_, handle_); });
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<Unit>
    {
        cancel_cb_.reset(); // owner 核同步析构
        if (state_ == async::WaitState::Cancelled)
            return Err(io_cancelled_error());
        if (!readiness_)
            return Err(std::move(readiness_).error());
        return Ok();
    }

private:
    static constexpr auto direction() -> async::IoEvent
    {
        return kWrite ? async::IoEvent::Write : async::IoEvent::Read;
    }

    R& reactor_;
    i32 fd_;
    std::coroutine_handle<> handle_ {};
    async::WaitState state_ = async::WaitState::Pending;
    std::stop_token token_ {};
    std::optional<std::stop_callback<std::function<void()>>> cancel_cb_ {};
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "io readiness not completed",
                                                 .location = std::source_location::current() });
};
} // namespace detail

class ConnectAwaiter {
public:
    ConnectAwaiter(async::EpollReactor& reactor, i32 fd) : reactor_(reactor), fd_(fd) { }

    auto set_cancellation(std::stop_token token) -> void { token_ = std::move(token); }

    [[nodiscard]] auto await_ready() noexcept -> bool { return false; }

    auto await_suspend(std::coroutine_handle<> h) -> bool
    {
        handle_ = h;
        auto reg = reactor_.register_write(fd_, h, nullptr, 0, &readiness_, &state_, async::WaitState::Ready);
        if (!reg) {
            readiness_ = Err(std::move(reg).error());
            return false;
        }
        if (token_.stop_possible())
            cancel_cb_.emplace(
                token_, [this] { detail::cancel_io_wait(reactor_, fd_, async::IoEvent::Write, state_, handle_); });
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<Unit>
    {
        cancel_cb_.reset();
        if (state_ == async::WaitState::Cancelled)
            return Err(detail::io_cancelled_error());
        if (!readiness_)
            return Err(std::move(readiness_).error());
        i32 err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            return Err(SystemError::from_errno());
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
    std::coroutine_handle<> handle_ {};
    async::WaitState state_ = async::WaitState::Pending;
    std::stop_token token_ {};
    std::optional<std::stop_callback<std::function<void()>>> cancel_cb_ {};
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "connect not completed",
                                                 .location = std::source_location::current() });
};

template<typename R>
class ReadReadyAwaiter : public detail::ReadyAwaiter<R, false> {
public:
    using detail::ReadyAwaiter<R, false>::ReadyAwaiter;
};

template<typename R>
class WriteReadyAwaiter : public detail::ReadyAwaiter<R, true> {
public:
    using detail::ReadyAwaiter<R, true>::ReadyAwaiter;
};

template<typename R>
ReadReadyAwaiter(R&, i32) -> ReadReadyAwaiter<R>;
template<typename R>
WriteReadyAwaiter(R&, i32) -> WriteReadyAwaiter<R>;

class TimedReadyAwaiter {
public:
    enum class Direction : u8 { Read, Write };

    TimedReadyAwaiter(async::EpollReactor& reactor, async::HierarchicalTimerWheel& timer, i32 fd, Direction direction,
                      async::Duration timeout)
        : reactor_(reactor), timer_(timer), fd_(fd), direction_(direction), timeout_(timeout)
    { }

    auto set_cancellation(std::stop_token token) -> void { token_ = std::move(token); }

    [[nodiscard]] auto await_ready() const noexcept -> bool { return false; }

    auto await_suspend(std::coroutine_handle<> h) -> bool
    {
        if (timeout_ <= async::Duration::zero()) {
            state_ = async::WaitState::TimedOut;
            return false;
        }
        handle_ = h;
        timer_id_ = timer_.schedule(timeout_, h, &state_, async::WaitState::TimedOut);
        auto reg = (direction_ == Direction::Read)
                       ? reactor_.register_read(fd_, h, nullptr, 0, &readiness_, &state_, async::WaitState::Ready)
                       : reactor_.register_write(fd_, h, nullptr, 0, &readiness_, &state_, async::WaitState::Ready);
        if (!reg) {
            (void)timer_.cancel(timer_id_);
            readiness_ = Err(std::move(reg).error());
            return false;
        }

        // 第三个竞争者：外部取消同时撤掉 timer 与 reactor 注册并唤醒（owner 核同步）。
        if (token_.stop_possible()) {
            cancel_cb_.emplace(token_, [this] {
                if (state_ != async::WaitState::Pending)
                    return;
                state_ = async::WaitState::Cancelled;
                (void)timer_.cancel(timer_id_);
                (void)reactor_.deregister(fd_, direction());
                if (async::g_event_loop && handle_)
                    async::g_event_loop->notify(handle_);
            });
        }
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<Unit>
    {
        cancel_cb_.reset();
        if (state_ == async::WaitState::Cancelled) {
            (void)timer_.cancel(timer_id_);
            return Err(detail::io_cancelled_error());
        }
        if (state_ == async::WaitState::TimedOut) {
            reactor_.deregister(fd_, direction());
            return Err(SystemError { .code = std::make_error_code(std::errc::timed_out),
                                     .message = direction_ == Direction::Read ? "read timeout" : "write timeout",
                                     .location = std::source_location::current() });
        }
        (void)timer_.cancel(timer_id_);
        if (!readiness_)
            return Err(std::move(readiness_).error());
        return Ok();
    }

private:
    static constexpr auto dir(Direction d) -> async::IoEvent
    {
        return d == Direction::Read ? async::IoEvent::Read : async::IoEvent::Write;
    }

    auto direction() const -> async::IoEvent { return dir(direction_); }

    async::EpollReactor& reactor_;
    async::HierarchicalTimerWheel& timer_;
    i32 fd_;
    Direction direction_;
    async::Duration timeout_;
    async::TimerId timer_id_ = 0;
    std::coroutine_handle<> handle_ {};
    async::WaitState state_ = async::WaitState::Pending;
    std::stop_token token_ {};
    std::optional<std::stop_callback<std::function<void()>>> cancel_cb_ {};
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "timed readiness not completed",
                                                 .location = std::source_location::current() });
};
} // namespace rain::net
