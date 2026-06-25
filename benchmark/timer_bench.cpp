// Rain 定时器微基准 —— schedule / cancel / tick(scan) 三阶段吞吐。
//
// 为什么需要它:echo 负载根本不触发 timer,所以 timer 的数据结构改造(任务2:slotmap)
// 无法用 echo 火焰图验证收益。本微基准是 timer 改造的 A/B 度量手段。
//
// 编译:g++ -std=c++23 -O2 -I src benchmark/timer_bench.cpp -o /tmp/timer_bench && /tmp/timer_bench
#include "async/timer.hpp"

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <vector>

using namespace rain::async;
using Sc = std::chrono::steady_clock;

int main()
{
    constexpr int N = 1'000'000;
    constexpr int kSpan = 262144; // 覆盖 level0/1/2 的延迟范围(ms)

    double best_sched = 1e18, best_cancel = 1e18, best_tick = 1e18;

    for (int rep = 0; rep < 3; ++rep) {
        HierarchicalTimerWheel wheel;
        std::vector<TimerId> ids;
        ids.reserve(N);
        auto h = std::noop_coroutine();

        // 阶段 1:schedule N 个定时器,延迟分布跨多个层级
        auto t0 = Sc::now();
        for (int i = 0; i < N; ++i)
            ids.push_back(wheel.schedule(std::chrono::milliseconds(1 + (i % kSpan)), h));
        auto t1 = Sc::now();

        // 阶段 2:取消一半(测 cancel 路径:旧实现 erase map 节点,新实现标记+空闲栈)
        int cancelled = 0;
        for (int i = 0; i < N; i += 2)
            cancelled += wheel.cancel(ids[i]) ? 1 : 0;
        auto t2 = Sc::now();

        // 阶段 3:推进到末尾,触发剩余、跳过已取消(测 tick 扫描:旧实现每 id 一次哈希查找)
        const auto target = wheel.current_time() + std::chrono::milliseconds(kSpan + 1);
        const std::size_t fired = wheel.advance_to(target);
        const auto drained = wheel.drain_ready();
        auto t3 = Sc::now();

        auto ns = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count(); };
        best_sched = std::min(best_sched, double(ns(t0, t1)) / N);
        best_cancel = std::min(best_cancel, double(ns(t1, t2)) / (N / 2));
        best_tick = std::min(best_tick, double(ns(t2, t3)) / N);
        std::printf("rep%d: schedule=%.1f cancel=%.1f tick=%.1f ns/op  (cancelled=%d fired=%zu drained=%zu)\n", rep,
            double(ns(t0, t1)) / N, double(ns(t1, t2)) / (N / 2), double(ns(t2, t3)) / N, cancelled, fired,
            drained.size());
    }

    std::printf("BEST: schedule=%.1f ns/op | cancel=%.1f ns/op | tick=%.1f ns/timer\n", best_sched, best_cancel,
        best_tick);
    return 0;
}
