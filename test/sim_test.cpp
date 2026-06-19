// SimReactor / SimLoop 测试：用注入式 reactor + 虚拟时钟，确定性地驱动 Rain 的「真实」
// 协程、真实就绪 awaiter（ReadReadyAwaiter，经 CTAD 实例化为 SimReactor 版）、真实
// 时间轮。就绪与时间完全由测试控制——无 fd、无 epoll、无 wall-clock——故 100% 可复现。
// 这正是「全运行时确定性仿真」：同样的原语，可控的调度。

#include "net/io_awaiters.hpp"

#include "async/task.hpp"
#include "async/timer.hpp"

#include "sim_loop.hpp"
#include "sim_reactor.hpp"
#include "testkit.hpp"

#include <chrono>

using namespace rain;
using namespace std::chrono;

// 真实协程：在 fd 上等待可读后置位（引用作协程参数，存于帧内，安全）。
static auto wait_read(test::SimReactor& rx, i32 fd, bool& done) -> async::Task<Unit>
{
    net::ReadReadyAwaiter ready(rx, fd); // CTAD -> ReadReadyAwaiter<SimReactor>
    (void)co_await ready;
    done = true;
    co_return unit;
}

// 真实协程：先睡 50ms（虚拟时钟），再等 fd 可读，分阶段记录进度。
static auto sleep_then_read(test::SimLoop& loop, i32 fd, int& stage) -> async::Task<Unit>
{
    (void)co_await async::sleep_for(loop.timer(), milliseconds(50));
    stage = 1;
    net::ReadReadyAwaiter ready(loop.reactor(), fd);
    (void)co_await ready;
    stage = 2;
    co_return unit;
}

auto main() -> int
{
    rain::test::Context ctx("sim");

    static_assert(async::IoReactorLike<rain::test::SimReactor>);

    // 1. I/O 就绪完全由注入确定性驱动：未注入则协程一直挂起，注入后恰好恢复一次。
    {
        test::SimLoop loop;
        bool done = false;
        loop.spawn(wait_read(loop.reactor(), 10, done));
        loop.run_ready(); // 协程跑到 co_await，挂起并注册到 reactor
        TK_CHECK(ctx, !done);
        loop.poll();
        loop.run_ready(); // 未注入就绪 -> 不恢复
        TK_CHECK(ctx, !done);
        loop.reactor().make_readable(10);
        loop.poll();
        loop.run_ready(); // 注入就绪 -> 恢复 -> 完成
        TK_CHECK(ctx, done);
        TK_CHECK(ctx, loop.pending() == 0);
    }

    // 2. 虚拟时钟 + I/O 集成：定时器与就绪在同一确定性时间线上协同推进。
    {
        test::SimLoop loop;
        int stage = 0;
        loop.spawn(sleep_then_read(loop, 11, stage));
        loop.run_ready();
        TK_CHECK(ctx, stage == 0); // 挂在 sleep 上
        loop.advance(40);
        loop.run_ready();
        TK_CHECK(ctx, stage == 0); // 不足 50ms
        loop.advance(15);
        loop.run_ready();
        TK_CHECK(ctx, stage == 1); // 越过 50ms -> 定时器触发 -> 推进到 read 挂起
        loop.reactor().make_readable(11);
        loop.poll();
        loop.run_ready();
        TK_CHECK(ctx, stage == 2);
        TK_CHECK(ctx, loop.pending() == 0);
    }

    // 3. 两个协程独立等待不同 fd：按注入顺序确定性地各自恢复。
    {
        test::SimLoop loop;
        bool a = false, b = false;
        loop.spawn(wait_read(loop.reactor(), 20, a));
        loop.spawn(wait_read(loop.reactor(), 21, b));
        loop.run_ready();
        loop.reactor().make_readable(20);
        loop.poll();
        loop.run_ready();
        TK_CHECK(ctx, a && !b); // 只有 fd 20 的协程恢复
        loop.reactor().make_readable(21);
        loop.poll();
        loop.run_ready();
        TK_CHECK(ctx, a && b);
        TK_CHECK(ctx, loop.pending() == 0);
    }

    // 4. 低层 SimReactor 机制：读写方向独立分派（与真实 reactor 的 M7 行为一致）。
    {
        test::SimReactor rx;
        Result<usize> r = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                            .message = "r",
                                            .location = std::source_location::current() });
        Result<usize> w = r;
        TK_CHECK(ctx, rx.register_read(30, {}, nullptr, 0, &r).has_value());
        TK_CHECK(ctx, rx.register_write(30, {}, nullptr, 0, &w).has_value());
        rx.make_writable(30);
        (void)rx.poll(milliseconds(0));
        TK_CHECK(ctx, w.has_value() && !r.has_value());
        rx.make_readable(30);
        (void)rx.poll(milliseconds(0));
        TK_CHECK(ctx, r.has_value());
    }

    return ctx.summary();
}
