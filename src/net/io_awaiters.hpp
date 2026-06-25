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

// 抢占式取消的共用收尾：被 stop_callback 触发，在 owner 核同步执行（与 reactor.poll/timer.tick
// 串行，故无需原子）。把 Pending 翻成 Cancelled、从 reactor 摘除该 fd 方向、调度唤醒挂起的协程。
namespace detail {
template<typename R>
inline auto cancel_io_wait(R& reactor, i32 fd, async::IoEvent dir, async::WaitState& state,
    std::coroutine_handle<> handle) -> void
{
    if (state != async::WaitState::Pending)
        return; // 已就绪/已超时，取消无效
    state = async::WaitState::Cancelled;
    (void)reactor.deregister(fd, dir); // 摘除，避免 reactor 也 fire 这个句柄
    if (async::g_event_loop && handle)
        async::g_event_loop->notify(handle); // 调度唤醒：下一轮事件循环 resume，await_resume 见 Cancelled
}

[[nodiscard]] inline auto io_cancelled_error() -> SystemError
{
    return SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                         .message = "io operation cancelled",
                         .location = std::source_location::current() };
}
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
            cancel_cb_.emplace(token_, [this] {
                detail::cancel_io_wait(reactor_, fd_, async::IoEvent::Write, state_, handle_);
            });
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<Unit>
    {
        cancel_cb_.reset();
        if (state_ == async::WaitState::Cancelled) {
            return Err(detail::io_cancelled_error());
        }
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
    std::coroutine_handle<> handle_ {};
    async::WaitState state_ = async::WaitState::Pending;
    std::stop_token token_ {};
    std::optional<std::stop_callback<std::function<void()>>> cancel_cb_ {};
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "connect not completed",
                                                 .location = std::source_location::current() });
};

// 就绪等待器对 reactor 类型泛化（CTAD 推导）：生产路径推导出 EpollReactor、生成与原先逐字节
// 相同的代码；测试可传入满足 IoReactorLike 的 SimReactor 做确定性仿真。
// set_cancellation 注入取消令牌后，挂 stop_callback：request_stop 时（owner 核同步）抢占唤醒。
template<typename R>
class ReadReadyAwaiter {
public:
    ReadReadyAwaiter(R& reactor, i32 fd) : reactor_(reactor), fd_(fd) { }

    auto set_cancellation(std::stop_token token) -> void { token_ = std::move(token); }

    [[nodiscard]] auto await_ready() noexcept -> bool { return false; }

    auto await_suspend(std::coroutine_handle<> h) -> bool
    {
        handle_ = h;
        // 带 state 注册：reactor 就绪做 Pending→Ready，与取消回调的 Pending→Cancelled 竞争（owner 核串行）。
        auto reg = reactor_.register_read(fd_, h, nullptr, 0, &readiness_, &state_, async::WaitState::Ready);
        if (!reg) {
            readiness_ = Err(std::move(reg).error());
            return false;
        }
        if (token_.stop_possible())
            cancel_cb_.emplace(token_, [this] {
                detail::cancel_io_wait(reactor_, fd_, async::IoEvent::Read, state_, handle_);
            });
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<Unit>
    {
        cancel_cb_.reset(); // 先析构 callback（owner 核同步，不阻塞）
        if (state_ == async::WaitState::Cancelled) {
            return Err(detail::io_cancelled_error());
        }
        if (!readiness_) {
            return Err(std::move(readiness_).error());
        }
        return Ok();
    }

private:
    R& reactor_;
    i32 fd_;
    std::coroutine_handle<> handle_ {};
    async::WaitState state_ = async::WaitState::Pending;
    std::stop_token token_ {};
    std::optional<std::stop_callback<std::function<void()>>> cancel_cb_ {};
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "read readiness not completed",
                                                 .location = std::source_location::current() });
};

template<typename R>
class WriteReadyAwaiter {
public:
    WriteReadyAwaiter(R& reactor, i32 fd) : reactor_(reactor), fd_(fd) { }

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
            cancel_cb_.emplace(token_, [this] {
                detail::cancel_io_wait(reactor_, fd_, async::IoEvent::Write, state_, handle_);
            });
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<Unit>
    {
        cancel_cb_.reset();
        if (state_ == async::WaitState::Cancelled) {
            return Err(detail::io_cancelled_error());
        }
        if (!readiness_) {
            return Err(std::move(readiness_).error());
        }
        return Ok();
    }

private:
    R& reactor_;
    i32 fd_;
    std::coroutine_handle<> handle_ {};
    async::WaitState state_ = async::WaitState::Pending;
    std::stop_token token_ {};
    std::optional<std::stop_callback<std::function<void()>>> cancel_cb_ {};
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

        // 外部取消（如 timeout(task) 取消整条链）：第三个竞争者——除 reactor 就绪/定时器超时外，
        // stop_callback 把 Pending→Cancelled、同时撤掉 timer 与 reactor 注册、唤醒。owner 核同步。
        if (token_.stop_possible())
            cancel_cb_.emplace(token_, [this] {
                if (state_ != async::WaitState::Pending)
                    return;
                state_ = async::WaitState::Cancelled;
                (void)timer_.cancel(timer_id_);
                (void)reactor_.deregister(
                    fd_, direction_ == Direction::Read ? async::IoEvent::Read : async::IoEvent::Write);
                if (async::g_event_loop && handle_)
                    async::g_event_loop->notify(handle_);
            });

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
            reactor_.deregister(fd_,
                                direction_ == Direction::Read ? async::IoEvent::Read : async::IoEvent::Write);
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
    std::coroutine_handle<> handle_ {};
    async::WaitState state_ = async::WaitState::Pending;
    std::stop_token token_ {};
    std::optional<std::stop_callback<std::function<void()>>> cancel_cb_ {};
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "timed readiness not completed",
                                                 .location = std::source_location::current() });
};

} // namespace rain::net
