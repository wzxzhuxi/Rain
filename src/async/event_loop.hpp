#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/concepts.hpp"
#include "async/io_reactor.hpp"
#include "async/signal.hpp"
#include "async/task.hpp"
#include "async/timer.hpp"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <ranges>
#include <stop_token>
#include <utility>
#include <vector>

#include <sys/eventfd.h>
#include <unistd.h>

namespace rain::async {

// ── 前向声明 ─────────────────────────────────────────────────────────

class EventLoop;

// ── 线程局部全局变量 ─────────────────────────────────────────────────

inline thread_local EpollReactor* g_reactor = nullptr;
inline thread_local HierarchicalTimerWheel* g_timer = nullptr;
inline thread_local EventLoop* g_event_loop = nullptr;

// ── 线程局部变量安装 RAII 守卫 ───────────────────────────────────────
// 确保即使走到错误路径，也会清理线程局部变量。

class ThreadLocalGuard {
public:
    explicit ThreadLocalGuard(EpollReactor& r, HierarchicalTimerWheel& t, EventLoop& l)
    {
        g_reactor = &r;
        g_timer = &t;
        g_event_loop = &l;
    }

    ~ThreadLocalGuard()
    {
        g_reactor = nullptr;
        g_timer = nullptr;
        g_event_loop = nullptr;
    }

    ThreadLocalGuard(const ThreadLocalGuard&) = delete;
    auto operator=(const ThreadLocalGuard&) -> ThreadLocalGuard& = delete;
};

// ── SignalAwaiter::await_suspend 定义 ───────────────────────────────
// 此处 EpollReactor 已完整定义，因此可调用 register_read。

inline auto SignalAwaiter::await_suspend(std::coroutine_handle<> h) -> bool
{
    auto reg = reactor_->register_read(signals_.fd(), h, nullptr, 0, &readiness_);
    if (!reg) {
        readiness_ = Err(std::move(reg).error());
        return false;
    }
    return true;
}

// ── 事件循环（EventLoop）─────────────────────────────────────────────
// 每核心单线程事件循环，驱动 I/O、定时器与信号。

class EventLoop {
public:
    // poll 超时由定时器 tick 时长推导
    static constexpr auto kDefaultPollTimeout = kTickDuration * 10;

    EventLoop(const EventLoop&) = delete;
    auto operator=(const EventLoop&) -> EventLoop& = delete;

    // 允许移动构造用于工厂返回。run() 进行中严禁移动，
    // 否则线程局部指针会悬空。删除移动赋值以避免
    // 误替换正在运行的循环对象。
    EventLoop(EventLoop&&) = default;
    auto operator=(EventLoop&&) -> EventLoop& = delete;

    ~EventLoop()
    {
        if (cross_core_fd_ >= 0) {
            ::close(cross_core_fd_);
        }
    }

    // -- 工厂 ----------------------------------------------------------

    [[nodiscard]] static auto create() -> Result<EventLoop>
    {
        auto reactor = EpollReactor::create();
        if (!reactor) {
            return Err(std::move(reactor).error());
        }

        const i32 wakeup_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd < 0) {
            return Err(SystemError::from_errno());
        }

        auto add_result = reactor->add(wakeup_fd, IoEvent::Read | IoEvent::EdgeTrig);
        if (!add_result) {
            ::close(wakeup_fd);
            return Err(std::move(add_result).error());
        }

        return Ok(EventLoop(std::move(*reactor), wakeup_fd));
    }

    // -- 信号支持 ------------------------------------------------------

    [[nodiscard]] auto add_signals(std::initializer_list<i32> signals) -> Result<Unit>
    {
        auto sigset = SignalSet::create(signals);
        if (!sigset) {
            return Err(std::move(sigset).error());
        }

        signals_.emplace(std::move(*sigset));
        return Ok();
    }

    [[nodiscard]] auto wait_signal() -> Result<SignalAwaiter>
    {
        if (!signals_) {
            return Err(SystemError { .code = std::make_error_code(std::errc::operation_not_permitted),
                                     .message = "signals not configured; call add_signals first",
                                     .location = std::source_location::current() });
        }
        return Ok(SignalAwaiter { *signals_, reactor_ });
    }

    [[nodiscard]] auto has_signals() const noexcept -> bool { return signals_.has_value(); }

    // -- Spawn（即发即弃）----------------------------------------------

    template<typename T>
    auto spawn(Task<T> task) -> Unit
    {
        auto handle = task.handle();
        enqueue(handle, std::move(task));
        return unit;
    }

    // -- 携带 JoinHandle 的 Spawn -------------------------------------

    template<typename T>
    [[nodiscard]] auto spawn_with_handle(Task<T> task) -> JoinHandle<T>
    {
        auto handle = task.handle();
        enqueue(handle, std::move(task));
        return JoinHandle<T>(handle);
    }

    // -- 跨核任务提交（线程安全）-----------------------------------------
    // 允许任意线程将任务提交到此 EventLoop。
    // 任务入队后通过 eventfd 唤醒目标核心的 epoll_wait。

    template<typename T>
    [[nodiscard]] auto submit(Task<T> task) -> Result<Unit>
    {
        auto handle = task.handle();
        {
            std::lock_guard lock(*cross_core_mutex_);
            cross_core_queue_.push_back({
                .handle = handle,
                .storage = [t = std::move(task)]() mutable { } });
        }
        const u64 val = 1;
        if (::write(cross_core_fd_, &val, sizeof(val)) != static_cast<isize>(sizeof(val))) {
            return Err(SystemError::from_errno());
        }
        return Ok();
    }

    // -- 主循环 --------------------------------------------------------

    [[nodiscard]] auto run() -> Result<Unit>
    {
        ThreadLocalGuard guard(reactor_, timer_, *this);

        while (!stop_source_.stop_requested()) {
            // 1. 恢复所有就绪协程
            auto ready = std::exchange(ready_queue_, { });
            std::ranges::for_each(
                ready | std::views::filter([](auto h) { return h && !h.done(); }),
                [](auto h) { h.resume(); });

            // 2. 回收已完成任务
            cleanup_completed();

            if (owned_tasks_.empty())
                break;

            // 3. 根据下一次定时器截止时间计算 poll 超时
            const auto now = Clock::now();
            auto timeout = kDefaultPollTimeout;

            if (const auto next = timer_.next_deadline()) {
                const auto remaining = *next - now;
                if (remaining <= Duration::zero()) {
                    timeout = Duration::zero();
                } else if (remaining < timeout) {
                    timeout = remaining;
                }
            }

            // 4. 轮询 I/O（包括 signalfd——它本质也是普通 fd）
            auto poll_result = reactor_.poll(timeout);
            if (!poll_result) {
                return Err(std::move(poll_result).error());
            }

            // 5. 取出 reactor 就绪的协程句柄
            auto reactor_ready = reactor_.drain_ready();
            ready_queue_.insert(ready_queue_.end(), reactor_ready.begin(), reactor_ready.end());

            // 6. 推进定时器并取出就绪句柄
            (void)timer_.advance_to(Clock::now());
            auto timer_ready = timer_.drain_ready();
            ready_queue_.insert(ready_queue_.end(), timer_ready.begin(), timer_ready.end());

            // 7. 排空跨核任务队列
            drain_cross_core();
        }

        return Ok();
    }

    // -- 便捷方法：运行单个主任务直至完成 ------------------------------

    [[nodiscard]] auto run_until_complete(Task<Unit> main_task) -> Result<Unit>
    {
        auto handle = main_task.handle();
        enqueue(handle, std::move(main_task));
        return run();
    }

    // -- 优雅关闭 ------------------------------------------------------

    auto request_stop() -> Unit
    {
        stop_source_.request_stop();
        return unit;
    }

    // -- 将协程句柄加入就绪队列（单线程，供组合子使用）------------------

    auto notify(std::coroutine_handle<> h) -> Unit
    {
        ready_queue_.push_back(h);
        return unit;
    }

    // -- 访问器 --------------------------------------------------------

    [[nodiscard]] auto reactor() -> EpollReactor& { return reactor_; }
    [[nodiscard]] auto timer() -> HierarchicalTimerWheel& { return timer_; }

private:
    explicit EventLoop(EpollReactor reactor, i32 cross_core_fd = -1)
        : reactor_(std::move(reactor)), cross_core_fd_(cross_core_fd)
    { }

    // 共享入队逻辑——消除 spawn/spawn_with_handle/run_until_complete 的重复实现。
    template<typename T>
    auto enqueue(std::coroutine_handle<typename Task<T>::promise_type> handle, Task<T> task) -> Unit
    {
        owned_tasks_.push_back({ .handle = handle, .storage = [t = std::move(task)]() mutable { } });
        ready_queue_.push_back(handle);
        return unit;
    }

    auto cleanup_completed() -> Unit
    {
        std::erase_if(owned_tasks_, [](const OwnedTask& t) { return !t.handle || t.handle.done(); });
        return unit;
    }

    auto drain_cross_core() -> Unit
    {
        std::lock_guard lock(*cross_core_mutex_);
        for (auto& task : cross_core_queue_) {
            ready_queue_.push_back(task.handle);
            owned_tasks_.push_back(std::move(task));
        }
        cross_core_queue_.clear();
        // 消费 eventfd 计数器
        u64 val = 0;
        (void)::read(cross_core_fd_, &val, sizeof(val));
        return unit;
    }

    struct OwnedTask {
        std::coroutine_handle<> handle;
        std::move_only_function<void()> storage; // 保持 Task<T> 生命周期
    };

    EpollReactor reactor_;
    HierarchicalTimerWheel timer_;
    std::optional<SignalSet> signals_;
    std::vector<OwnedTask> owned_tasks_;
    std::vector<std::coroutine_handle<>> ready_queue_;
    std::stop_source stop_source_;

    // 跨核任务提交
    i32 cross_core_fd_ = -1;
    Box<std::mutex> cross_core_mutex_ = std::make_unique<std::mutex>();
    std::vector<OwnedTask> cross_core_queue_;
};

} // namespace rain::async
