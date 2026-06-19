#pragma once

// SimLoop —— 确定性测试驱动：用 SimReactor + 虚拟时钟时间轮驱动 Rain 的「真实」
// Task 与就绪 awaiter。它镜像 EventLoop::run() 的驱动逻辑，但每一步都由测试显式触发：
//   run_ready() 恢复就绪协程；poll() 据注入的就绪触发 reactor；advance() 推进虚拟时间。
// 无 epoll、无 wall-clock、无线程 —— 同一序列 100% 可复现。
//
// 它不复用 EventLoop::run() 本身（后者与 epoll_wait/Clock::now() 耦合），而是以相同的
// 原语（Task、就绪 awaiter、HierarchicalTimerWheel）复刻其调度，从而在不动生产 EventLoop、
// 不破坏依赖具体 g_event_loop 的自由函数 API 的前提下，获得完全确定的运行时仿真。

#include "core/types.hpp"

#include "async/task.hpp"
#include "async/timer.hpp"

#include "sim_reactor.hpp"

#include <coroutine>
#include <functional>
#include <utility>
#include <vector>

namespace rain::test {

class SimLoop {
public:
    [[nodiscard]] auto reactor() -> SimReactor& { return reactor_; }
    [[nodiscard]] auto timer() -> async::HierarchicalTimerWheel& { return timer_; }

    template<typename T>
    auto spawn(async::Task<T> task) -> void
    {
        auto h = task.handle();
        owned_.push_back({ h, [t = std::move(task)]() mutable { } });
        ready_.push_back(h);
    }

    // 恢复当前所有就绪协程（快照），随后回收已完成任务（销毁其帧）。
    auto run_ready() -> void
    {
        auto batch = std::exchange(ready_, {});
        for (auto h : batch) {
            if (h && !h.done())
                h.resume();
        }
        std::erase_if(owned_, [](const Owned& o) { return !o.handle || o.handle.done(); });
    }

    // 触发 reactor：仅被 make_readable/make_writable 注入的就绪会触发，恢复对应 awaiter。
    auto poll() -> void
    {
        (void)reactor_.poll(async::Duration::zero());
        auto r = reactor_.drain_ready();
        ready_.insert(ready_.end(), r.begin(), r.end());
    }

    // 推进虚拟时间 ticks 毫秒，把到期定时器的句柄并入下一轮就绪。
    auto advance(int ticks) -> void
    {
        for (int i = 0; i < ticks; ++i)
            (void)timer_.tick();
        auto t = timer_.drain_ready();
        ready_.insert(ready_.end(), t.begin(), t.end());
    }

    [[nodiscard]] auto pending() const -> usize { return owned_.size(); }

private:
    struct Owned {
        std::coroutine_handle<> handle;
        std::move_only_function<void()> storage; // 保持 Task<T> 生命周期
    };

    SimReactor reactor_;
    async::HierarchicalTimerWheel timer_;
    std::vector<Owned> owned_;
    std::vector<std::coroutine_handle<>> ready_;
};

} // namespace rain::test
