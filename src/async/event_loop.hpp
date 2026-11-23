#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/concepts.hpp"
#include "async/io_reactor.hpp"
#include "async/task.hpp"
#include "async/timer.hpp"

#include <algorithm>
#include <functional>
#include <mutex>
#include <stop_token>
#include <utility>
#include <vector>

#include <sys/eventfd.h>
#include <unistd.h>

namespace rain::async {
class EventLoop;
inline thread_local EpollReactor* g_reactor = nullptr;
inline thread_local HierarchicalTimerWheel* g_timer = nullptr;
inline thread_local EventLoop* g_event_loop = nullptr;

// 线程局部指针安装守卫：即使走错误路径也会清理。
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

// 每核单线程事件循环：驱动 ready 队列、epoll、定时器与跨核提交。
class EventLoop {
public:
    static constexpr auto kDefaultPollTimeout = kTickDuration * 10;
    EventLoop(const EventLoop&) = delete;
    auto operator=(const EventLoop&) -> EventLoop& = delete;

    // 允许 move 构造以便工厂返回；run() 期间严禁移动。裸 fd 必须置空源对象以防双重 close。
    EventLoop(EventLoop&& other) noexcept
        : reactor_(std::move(other.reactor_)), timer_(std::move(other.timer_)),
          owned_tasks_(std::move(other.owned_tasks_)),
          ready_queue_(std::move(other.ready_queue_)), resume_scratch_(std::move(other.resume_scratch_)),
          stop_source_(std::move(other.stop_source_)), graceful_source_(std::move(other.graceful_source_)),
          cross_core_fd_(std::exchange(other.cross_core_fd_, -1)),
          cross_core_mutex_(std::move(other.cross_core_mutex_)),
          cross_core_queue_(std::move(other.cross_core_queue_))
    { }
    auto operator=(EventLoop&&) -> EventLoop& = delete;

    ~EventLoop()
    {
        if (cross_core_fd_ >= 0)
            ::close(cross_core_fd_);
    }

    [[nodiscard]] static auto create() -> Result<EventLoop>
    {
        auto reactor = EpollReactor::create();
        if (!reactor)
            return Err(std::move(reactor).error());
        const i32 wakeup_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd < 0)
            return Err(SystemError::from_errno());

        if (auto added = reactor->add(wakeup_fd, IoEvent::Read | IoEvent::EdgeTrig); !added) {
            ::close(wakeup_fd);
            return Err(std::move(added).error());
        }
        return Ok(EventLoop(std::move(*reactor), wakeup_fd));
    }

    template<typename T>
    auto spawn(Task<T> task) -> Unit
    {
        auto handle = task.handle();
        enqueue(handle, std::move(task));
        return unit;
    }

    template<typename T>
    [[nodiscard]] auto spawn_with_handle(Task<T> task) -> JoinHandle<T>
    {
        auto handle = task.handle();
        enqueue(handle, std::move(task));
        return JoinHandle<T>(handle);
    }

    // 服务循环（如 accept loop）注入优雅停机令牌：停机时其挂起 I/O 被抢占取消。
    template<typename T>
    auto spawn_service(Task<T> task) -> Unit
    {
        task.set_cancellation(graceful_source_.get_token());
        return spawn(std::move(task));
    }

    // 任意线程可提交；eventfd 唤醒目标核。
    template<typename T>
    [[nodiscard]] auto submit(Task<T> task) -> Result<Unit>
    {
        auto handle = task.handle();
        {
            std::lock_guard lock(*cross_core_mutex_);
            cross_core_queue_.push_back({ .handle = handle, .storage = [t = std::move(task)]() mutable { } });
        }
        const u64 val = 1;
        if (::write(cross_core_fd_, &val, sizeof(val)) != static_cast<isize>(sizeof(val)))
            return Err(SystemError::from_errno());
        return Ok();
    }

    [[nodiscard]] auto run() -> Result<Unit>
    {
        ThreadLocalGuard guard(reactor_, timer_, *this);

        while (!stop_source_.stop_requested()) {
            // 1. 恢复就绪协程（swap 到复用缓冲，保留容量）
            resume_scratch_.clear();
            std::swap(resume_scratch_, ready_queue_);
            for (auto h : resume_scratch_)
                if (h && !h.done())
                    h.resume();

            // 2. 回收已完成任务
            cleanup_completed();

            // 退出前排空跨核队列，避免并发 submit 被静默丢弃
            if (owned_tasks_.empty()) {
                drain_cross_core();
                if (owned_tasks_.empty())
                    break;
            }

            // 3. poll 超时取下一次定时器截止时间
            const auto now = Clock::now();
            auto timeout = kDefaultPollTimeout;
            if (const auto next = timer_.next_deadline()) {
                const auto remaining = *next - now;
                timeout = remaining <= Duration::zero() ? Duration::zero() : std::min(remaining, timeout);
            }

            // 4. 轮询 I/O
            auto poll_result = reactor_.poll(timeout);
            if (!poll_result)
                return Err(std::move(poll_result).error());

            // 5. 取出就绪句柄，推进定时器，排空跨核队列
            reactor_.drain_ready_into(ready_queue_);
            (void)timer_.advance_to(Clock::now());
            timer_.drain_ready_into(ready_queue_);
            drain_cross_core();
        }
        return Ok();
    }

    [[nodiscard]] auto run_until_complete(Task<Unit> main_task) -> Result<Unit>
    {
        auto handle = main_task.handle();
        enqueue(handle, std::move(main_task));
        return run();
    }

    auto request_stop() -> Unit
    {
        stop_source_.request_stop();
        wake_cross_core();
        return unit;
    }

    [[nodiscard]] auto shutdown_token() -> std::stop_token { return graceful_source_.get_token(); }

    // 停 accept、不硬停 run()：在途普通 spawn 继续 drain，超时硬停由 Executor 负责。
    auto request_graceful_stop() -> Unit
    {
        graceful_source_.request_stop();
        wake_cross_core();
        return unit;
    }

    auto notify(std::coroutine_handle<> h) -> Unit
    {
        ready_queue_.push_back(h);
        return unit;
    }

    [[nodiscard]] auto reactor() -> EpollReactor& { return reactor_; }
    [[nodiscard]] auto timer() -> HierarchicalTimerWheel& { return timer_; }

private:
    explicit EventLoop(EpollReactor reactor, i32 cross_core_fd = -1)
        : reactor_(std::move(reactor)), cross_core_fd_(cross_core_fd)
    { }

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

    auto wake_cross_core() -> void
    {
        if (cross_core_fd_ >= 0) {
            const u64 val = 1;
            (void)::write(cross_core_fd_, &val, sizeof(val));
        }
    }

    auto drain_cross_core() -> Unit
    {
        std::lock_guard lock(*cross_core_mutex_);
        for (auto& task : cross_core_queue_) {
            ready_queue_.push_back(task.handle);
            owned_tasks_.push_back(std::move(task));
        }
        cross_core_queue_.clear();
        u64 val = 0;
        (void)::read(cross_core_fd_, &val, sizeof(val));
        return unit;
    }

    struct OwnedTask {
        std::coroutine_handle<> handle;
        std::move_only_function<void()> storage; // 持有 Task 生命周期
    };
    EpollReactor reactor_;
    HierarchicalTimerWheel timer_;
    std::vector<OwnedTask> owned_tasks_;
    std::vector<std::coroutine_handle<>> ready_queue_;
    std::vector<std::coroutine_handle<>> resume_scratch_;
    std::stop_source stop_source_;
    std::stop_source graceful_source_;

    // 跨核提交字段：eventfd 唤醒目标核；其余字段只在锁内或 owner 核访问。
    i32 cross_core_fd_ = -1;
    Box<std::mutex> cross_core_mutex_ = std::make_unique<std::mutex>();
    std::vector<OwnedTask> cross_core_queue_;
};
} // namespace rain::async
