// 时间轮测试：虚拟时钟驱动，完全确定、无 sleep。
// 覆盖 C3（级联相位对齐）与 M3（急切取消、无墓碑增长、不解引用已销毁 state）。

#include "async/timer.hpp"
#include "testkit.hpp"

#include <chrono>

using namespace rain;
using namespace rain::async;
using namespace std::chrono;

// 在给定相位调度一个 delay 后的定时器，返回它实际触发时相对调度点的 tick 数。
static auto fire_offset(int phase, int delay) -> int
{
    HierarchicalTimerWheel w;
    (void)w.advance_to(w.current_time() + milliseconds(phase));
    WaitState st = WaitState::Pending;
    (void)w.schedule_at(w.current_time() + milliseconds(delay), {}, &st);
    for (int i = 1; i <= delay + 600; ++i) {
        (void)w.tick();
        if (st != WaitState::Pending) {
            return i;
        }
    }
    return -1;
}

auto main() -> int
{
    rain::test::Context ctx("timer");

    // C3 属性：任取相位与 delay，定时器都在其 deadline 的 +1 tick（良性校准偏移）触发，
    // 绝不晚整整一圈。这条属性正是当年抓出 C3 的全相位仿真的固化。
    for (int phase : { 0, 1, 7, 63, 100, 127, 200, 255 }) {
        for (int delay : { 1, 5, 50, 200, 255, 256, 300, 511, 1000, 5000, 20000 }) {
            const int f = fire_offset(phase, delay);
            TK_CHECK(ctx, f >= delay && f <= delay + 2);
        }
    }

    // 未取消的定时器会触发。
    {
        HierarchicalTimerWheel w;
        WaitState st = WaitState::Pending;
        (void)w.schedule_at(w.current_time() + milliseconds(50), {}, &st);
        for (int i = 0; i < 100; ++i)
            (void)w.tick();
        TK_CHECK(ctx, st != WaitState::Pending);
    }

    // M3：取消后不再触发，且 cancel 返回 true。
    {
        HierarchicalTimerWheel w;
        WaitState st = WaitState::Pending;
        auto id = w.schedule_at(w.current_time() + milliseconds(50), {}, &st);
        TK_CHECK(ctx, w.cancel(id));
        for (int i = 0; i < 100; ++i)
            (void)w.tick();
        TK_CHECK(ctx, st == WaitState::Pending);
        // 重复取消应返回 false（已不在表中）。
        TK_CHECK(ctx, !w.cancel(id));
    }

    // 多层定时器各自在自己的 deadline 触发。
    {
        HierarchicalTimerWheel w;
        WaitState a = WaitState::Pending, b = WaitState::Pending, c = WaitState::Pending;
        (void)w.schedule_at(w.current_time() + milliseconds(10), {}, &a);
        (void)w.schedule_at(w.current_time() + milliseconds(300), {}, &b);
        (void)w.schedule_at(w.current_time() + milliseconds(20000), {}, &c);
        for (int i = 0; i < 11; ++i)
            (void)w.tick();
        TK_CHECK(ctx, a != WaitState::Pending && b == WaitState::Pending && c == WaitState::Pending);
        for (int i = 0; i < 295; ++i)
            (void)w.tick();
        TK_CHECK(ctx, b != WaitState::Pending && c == WaitState::Pending);
    }

    // M3 关键安全性：取消一个指向堆上 state 的远期定时器，释放该 state，再推进越过 deadline。
    // 若 cancel 没有急切移除条目，扫描时会解引用已释放内存——ASan 下变成硬失败。
    {
        HierarchicalTimerWheel w;
        auto* st = new WaitState(WaitState::Pending);
        auto id = w.schedule_at(w.current_time() + milliseconds(5000), {}, st);
        TK_CHECK(ctx, w.cancel(id));
        delete st;
        for (int i = 0; i < 6000; ++i)
            (void)w.tick();
    }

    return ctx.summary();
}
