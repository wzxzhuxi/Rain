#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/concepts.hpp"
#include "async/io_reactor.hpp"
#include "async/task.hpp"
#include "runtime/thread_pool.hpp"

#include <concepts>
#include <coroutine>
#include <sys/eventfd.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace rain::runtime {

// ── 跨线程通知器（CrossThreadNotifier）───────────────────────────────
// Linux eventfd 的轻量 RAII 封装，用于跨线程唤醒。
// 使用场景：AsyncSink、spawn_blocking、Executor 关闭流程。
//
// eventfd 是一个 8 字节内核计数器。write() 递增计数，
// read() 读取并清零。计数非零时，epoll 会报告可读。
// 相比 pipe（有缓冲区且需要两个 fd），它更轻量且语义更清晰，
// 专门用于事件通知。

class CrossThreadNotifier {
public:
    CrossThreadNotifier(const CrossThreadNotifier&) = delete;
    auto operator=(const CrossThreadNotifier&) -> CrossThreadNotifier& = delete;

    CrossThreadNotifier(CrossThreadNotifier&& other) noexcept : fd_(std::exchange(other.fd_, -1)) { }

    auto operator=(CrossThreadNotifier&& other) noexcept -> CrossThreadNotifier&
    {
        if (this != &other) {
            if (auto close_result = close(); !close_result) {
                // 移动赋值清理路径仅做 best-effort。
            }
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~CrossThreadNotifier()
    {
        if (auto close_result = close(); !close_result) {
            // 析构路径仅做 best-effort。
        }
    }

    [[nodiscard]] static auto create() -> Result<CrossThreadNotifier>
    {
        const i32 fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd < 0) {
            return Err(SystemError::from_errno());
        }
        return Ok(CrossThreadNotifier(fd));
    }

    // eventfd 描述符，可在 epoll 中以 IoEvent::Read | IoEvent::EdgeTrig 注册。
    [[nodiscard]] auto fd() const noexcept -> i32 { return fd_; }

    // 递增计数器（唤醒 EventLoop 线程中的 epoll_wait）。
    // 线程安全：可从任意线程调用。
    [[nodiscard]] auto notify() -> Result<Unit>
    {
        const u64 val = 1;
        if (::write(fd_, &val, sizeof(val)) != static_cast<isize>(sizeof(val))) {
            return Err(SystemError::from_errno());
        }
        return Ok();
    }

    // 读取并重置计数器（消费通知）。
    // 在 epoll 报告可读后由 EventLoop 线程调用。
    [[nodiscard]] auto acknowledge() -> Result<u64>
    {
        u64 val = 0;
        if (::read(fd_, &val, sizeof(val)) != static_cast<isize>(sizeof(val))) {
            if (errno == EAGAIN) {
                return Ok(u64 { 0 }); // 当前没有待处理通知
            }
            return Err(SystemError::from_errno());
        }
        return Ok(val);
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return fd_ >= 0; }

    [[nodiscard]] auto close() -> Result<Unit>
    {
        if (fd_ < 0) {
            return Ok();
        }
        if (::close(fd_) < 0) {
            fd_ = -1;
            return Err(SystemError::from_errno());
        }
        fd_ = -1;
        return Ok();
    }

private:
    explicit CrossThreadNotifier(i32 fd) : fd_(fd) { }

    i32 fd_ = -1;
};

// ── 阻塞等待器（BlockingAwaiter）─────────────────────────────────────
// spawn_blocking() 返回的 awaiter。其 await_suspend 过程：
//   1. 将 eventfd 注册到 reactor（仅就绪通知）
//   2. 把 fn 提交到 ThreadPool
//   3. worker 执行 fn，写入结果并通知 eventfd
//   4. reactor 被唤醒并恢复协程
//   5. await_resume 读取结果
//
// 该 awaiter 持有 CrossThreadNotifier 和结果存储。
// 它位于协程帧（栈）中，因此其成员指针在挂起/恢复边界内保持有效。

template<typename T>
class BlockingAwaiter {
public:
    BlockingAwaiter(async::EpollReactor& reactor, ThreadPool& pool, CrossThreadNotifier notifier)
        : reactor_(reactor), pool_(pool), notifier_(std::move(notifier))
    { }

    [[nodiscard]] auto await_ready() noexcept -> bool { return false; }

    template<typename F>
    auto set_work(F&& fn) -> Unit
    {
        work_ = [this, f = std::forward<F>(fn)]() mutable {
            try {
                if constexpr (std::same_as<T, Unit>) {
                    f();
                    result_ = Ok(unit);
                } else {
                    result_ = Ok(T(f()));
                }
            } catch (...) {
                result_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_not_permitted),
                                            .message = "exception in spawn_blocking task",
                                            .location = std::source_location::current() });
            }
            // 唤醒 EventLoop 线程
            if (auto notify_result = notifier_.notify(); !notify_result && result_) {
                result_ = Err(std::move(notify_result).error());
            }
        };
        return unit;
    }

    auto await_suspend(std::coroutine_handle<> h) -> bool
    {
        // 向 reactor 注册 eventfd，仅监听就绪（无缓冲区）
        auto reg = reactor_.register_read(notifier_.fd(), h, nullptr, 0, &readiness_);
        if (!reg) {
            result_ = Err(std::move(reg).error());
            return false; // 不挂起，await_resume 直接读取错误
        }

        // 向 ThreadPool 提交任务
        auto spawn_result = pool_.spawn(std::move(work_));
        if (!spawn_result) {
            // 线程池已关闭：取消注册并且不挂起
            reactor_.deregister(notifier_.fd());
            result_ = Err(std::move(spawn_result).error());
            return false;
        }
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<T>
    {
        // 确认 eventfd（消费通知）
        auto ack_result = notifier_.acknowledge();
        if (!ack_result) {
            reactor_.deregister(notifier_.fd());
            return Err(std::move(ack_result).error());
        }

        // 从 reactor 取消注册
        reactor_.deregister(notifier_.fd());

        return std::move(result_);
    }

private:
    async::EpollReactor& reactor_;
    ThreadPool& pool_;
    CrossThreadNotifier notifier_;
    std::move_only_function<void()> work_;
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "blocking task not completed",
                                                 .location = std::source_location::current() });
    Result<T> result_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                          .message = "blocking task not completed",
                                          .location = std::source_location::current() });
};

// ── 阻塞任务桥接（spawn_blocking）────────────────────────────────────
// 将 CPU 密集型函数卸载到 ThreadPool，返回可 co_await 的 Task，
// 在任务完成后恢复到 EventLoop 线程继续执行。
//
// 用法：
//   auto result = co_await spawn_blocking(reactor, pool, [] {
//       return expensive_computation();
//   });

template<typename F>
    requires std::invocable<F>
[[nodiscard]] auto spawn_blocking(async::EpollReactor& reactor, ThreadPool& pool, F&& fn)
    -> async::Task<Result<std::invoke_result_t<F>>>
{
    using T = std::invoke_result_t<F>;
    using ResultT = std::conditional_t<std::same_as<T, void>, Unit, T>;

    auto notifier = CrossThreadNotifier::create();
    if (!notifier) {
        co_return Err(std::move(notifier).error());
    }

    BlockingAwaiter<ResultT> awaiter(reactor, pool, std::move(*notifier));

    if constexpr (std::same_as<T, void>) {
        awaiter.set_work([f = std::forward<F>(fn)]() mutable { f(); });
    } else {
        awaiter.set_work(std::forward<F>(fn));
    }

    auto result = co_await awaiter;

    co_return std::move(result);
}

} // namespace rain::runtime
