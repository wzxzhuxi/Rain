#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/concepts.hpp"
#include "async/io_reactor.hpp"
#include "async/task.hpp"
#include "runtime/thread_pool.hpp"

#include <concepts>
#include <coroutine>
#include <memory>
#include <sys/eventfd.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace rain::runtime {
// eventfd 轻量 RAII：写递增计数，读清零；计数非零时 epoll 报告可读。
class CrossThreadNotifier {
public:
    CrossThreadNotifier(const CrossThreadNotifier&) = delete;
    auto operator=(const CrossThreadNotifier&) -> CrossThreadNotifier& = delete;

    CrossThreadNotifier(CrossThreadNotifier&& other) noexcept : fd_(std::exchange(other.fd_, -1)) { }

    auto operator=(CrossThreadNotifier&& other) noexcept -> CrossThreadNotifier&
    {
        if (this != &other) {
            (void)close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~CrossThreadNotifier() { (void)close(); }

    [[nodiscard]] static auto create() -> Result<CrossThreadNotifier>
    {
        const i32 fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd < 0)
            return Err(SystemError::from_errno());
        return Ok(CrossThreadNotifier(fd));
    }

    [[nodiscard]] auto fd() const noexcept -> i32 { return fd_; }

    // 任意线程可调用。
    [[nodiscard]] auto notify() -> Result<Unit>
    {
        const u64 val = 1;
        if (::write(fd_, &val, sizeof(val)) != static_cast<isize>(sizeof(val)))
            return Err(SystemError::from_errno());
        return Ok();
    }

    // 消费通知；无待处理通知时返回 0。
    [[nodiscard]] auto acknowledge() -> Result<u64>
    {
        u64 val = 0;
        if (::read(fd_, &val, sizeof(val)) != static_cast<isize>(sizeof(val))) {
            if (errno == EAGAIN)
                return Ok(u64 { 0 });
            return Err(SystemError::from_errno());
        }
        return Ok(val);
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return fd_ >= 0; }

    [[nodiscard]] auto close() -> Result<Unit>
    {
        if (fd_ < 0)
            return Ok();
        const i32 fd = std::exchange(fd_, -1);
        if (::close(fd) < 0)
            return Err(SystemError::from_errno());
        return Ok();
    }

private:
    explicit CrossThreadNotifier(i32 fd) : fd_(fd) { }

    i32 fd_ = -1;
};

// worker 写回的结果与 eventfd 放进 refcounted holder：awaiter（帧）与 worker 闭包各持一份，
// 即使协程帧在 worker 写回前销毁，也不会 UAF。
template<typename T>
struct BlockingState {
    explicit BlockingState(CrossThreadNotifier n) : notifier(std::move(n)) { }
    CrossThreadNotifier notifier;
    Result<T> result = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                         .message = "blocking task not completed",
                                         .location = std::source_location::current() });
};

template<typename T>
class BlockingAwaiter {
public:
    BlockingAwaiter(async::EpollReactor& reactor, ThreadPool& pool, CrossThreadNotifier notifier)
        : reactor_(reactor), pool_(pool), state_(std::make_shared<BlockingState<T>>(std::move(notifier)))
    { }

    [[nodiscard]] auto await_ready() noexcept -> bool { return false; }

    template<typename F>
    auto set_work(F&& fn) -> Unit
    {
        work_ = [state = state_, f = std::forward<F>(fn)]() mutable {
            try {
                if constexpr (std::same_as<T, Unit>) {
                    f();
                    state->result = Ok(unit);
                } else {
                    state->result = Ok(T(f()));
                }
            } catch (...) {
                state->result = Err(SystemError { .code = std::make_error_code(std::errc::operation_not_permitted),
                                                  .message = "exception in spawn_blocking task",
                                                  .location = std::source_location::current() });
            }
            if (auto notified = state->notifier.notify(); !notified && state->result)
                state->result = Err(std::move(notified).error());
        };
        return unit;
    }

    auto await_suspend(std::coroutine_handle<> h) -> bool
    {
        auto reg = reactor_.register_read(state_->notifier.fd(), h, nullptr, 0, &readiness_);
        if (!reg) {
            state_->result = Err(std::move(reg).error());
            return false;
        }
        auto spawned = pool_.spawn(std::move(work_));
        if (!spawned) {
            reactor_.deregister(state_->notifier.fd());
            state_->result = Err(std::move(spawned).error());
            return false;
        }
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<T>
    {
        auto ack = state_->notifier.acknowledge();
        reactor_.deregister(state_->notifier.fd());
        if (!ack)
            return Err(std::move(ack).error());
        return std::move(state_->result);
    }

private:
    async::EpollReactor& reactor_;
    ThreadPool& pool_;
    std::shared_ptr<BlockingState<T>> state_;
    std::move_only_function<void()> work_;
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "blocking task not completed",
                                                 .location = std::source_location::current() });
};

// 把阻塞函数卸载到 ThreadPool，完成后经 eventfd 唤醒原事件循环。
template<typename F>
    requires std::invocable<F>
[[nodiscard]] auto spawn_blocking(async::EpollReactor& reactor, ThreadPool& pool, F&& fn) -> async::Task<
    Result<std::conditional_t<std::is_void_v<std::invoke_result_t<F>>, Unit, std::invoke_result_t<F>>>>
{
    using T = std::invoke_result_t<F>;
    using ResultT = std::conditional_t<std::same_as<T, void>, Unit, T>;
    auto notifier = CrossThreadNotifier::create();
    if (!notifier)
        co_return Err(std::move(notifier).error());
    BlockingAwaiter<ResultT> awaiter(reactor, pool, std::move(*notifier));
    if constexpr (std::same_as<T, void>)
        awaiter.set_work([f = std::forward<F>(fn)]() mutable { f(); });
    else
        awaiter.set_work(std::forward<F>(fn));
    co_return co_await awaiter;
}
} // namespace rain::runtime
