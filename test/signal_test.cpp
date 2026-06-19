// 信号测试：M4——多核下进程定向信号必须被 signalfd 接住，而非落到未屏蔽的
// worker 上执行默认动作（终止进程）。signalfd 故意挂在非首核上，正是逃逸易发的情形。
// 该测试是确定性的：信号在所有线程被屏蔽 -> 进入进程待决集 -> 由 core1 的 signalfd 读取。

#include "runtime/runtime.hpp"

#include "testkit.hpp"

#include <atomic>
#include <csignal>
#include <unistd.h>

using namespace rain;
using namespace rain::async;

// 引用以参数传入协程帧（避免有捕获 lambda 协程的悬垂闭包）。
static auto sig_task(std::atomic<bool>& handled, EventLoop& loop) -> Task<Unit>
{
    if (auto aw = loop.wait_signal()) {
        (void)co_await *aw;
        handled.store(true);
    }
    loop.request_stop();
    co_return unit;
}

auto main() -> int
{
    rain::test::Context ctx("signal");

    auto rt = runtime::Runtime::builder().worker_threads(2).enable_blocking(false).build();
    TK_CHECK(ctx, rt.has_value());
    if (!rt)
        return ctx.summary();

    std::atomic<bool> handled { false };
    auto rr = rt->run([&handled](EventLoop& loop, usize core) -> Result<Unit> {
        if (core == 1) {
            if (auto a = loop.add_signals({ SIGUSR1 }); !a)
                return Err(std::move(a).error());
            loop.spawn(sig_task(handled, loop));
        }
        return Ok();
    });
    TK_CHECK(ctx, rr.has_value());
    if (!rr) {
        rt->stop_and_join();
        return ctx.summary();
    }

    (void)::kill(::getpid(), SIGUSR1);
    rt->join(); // core1 处理信号后 request_stop -> worker 退出 -> join 返回（无需 sleep）

    TK_CHECK(ctx, handled.load());
    return ctx.summary();
}
