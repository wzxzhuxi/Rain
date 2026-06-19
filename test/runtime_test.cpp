// Runtime 测试：协程生命周期与执行入口。在 ASan 下运行时，这些就是
// C1/C4/M1/M2/M5/M6 以及 when_all/when_any UAF 的回归门。
// 断言覆盖可观察行为；UAF/竞争由 sanitizer 兜底。

#include "async/combinators.hpp"
#include "runtime/runtime.hpp"

#include "testkit.hpp"

#include <vector>

using namespace rain;
using namespace rain::async;

static auto err(const char* m) -> SystemError
{
    return SystemError { .code = std::make_error_code(std::errc::io_error), .message = m,
                         .location = std::source_location::current() };
}
static auto ok_task() -> Task<Result<int>> { co_return Ok(42); }
static auto unit_task() -> Task<Result<Unit>> { co_return Ok(); }
static auto fail_task() -> Task<Result<int>> { co_return Err(err("boom")); }
static auto succeed_task(int v) -> Task<Result<int>> { co_return Ok(v); }

auto main() -> int
{
    rain::test::Context ctx("runtime");

    auto rt = runtime::Runtime::builder().worker_threads(1).enable_blocking(true).blocking_threads(2).build();
    TK_CHECK(ctx, rt.has_value());
    if (!rt)
        return ctx.summary();

    // C1 / M5+M6：block_on 不在帧销毁后读 promise，对 Result<int>/Result<Unit> 都返回正确值。
    {
        auto r = rt->block_on(ok_task());
        TK_CHECK(ctx, r.has_value() && *r == 42);
        TK_CHECK(ctx, rt->block_on(unit_task()).has_value());
    }

    // M1：spawn_blocking 走非阻塞入队，结果正确回传。
    {
        auto r = rt->block_on([]() -> Task<Result<int>> {
            co_return co_await runtime::spawn_blocking([] { return 21 * 2; });
        }());
        TK_CHECK(ctx, r.has_value() && *r == 42);
    }

    // C4：when_any 全部失败时返回 Err 且不挂起。
    {
        auto r = rt->block_on([]() -> Task<Result<int>> {
            std::vector<Task<Result<int>>> ts;
            ts.push_back(fail_task());
            ts.push_back(fail_task());
            ts.push_back(fail_task());
            co_return co_await when_any(std::move(ts));
        }());
        TK_CHECK(ctx, !r.has_value());
    }

    // C4：when_any 有一个成功时返回该成功值。
    {
        auto r = rt->block_on([]() -> Task<Result<int>> {
            std::vector<Task<Result<int>>> ts;
            ts.push_back(fail_task());
            ts.push_back(succeed_task(7));
            ts.push_back(fail_task());
            co_return co_await when_any(std::move(ts));
        }());
        TK_CHECK(ctx, r.has_value() && *r == 7);
    }

    // when_all：按序收集全部结果，无 UAF。
    {
        auto r = rt->block_on([]() -> Task<Result<int>> {
            std::vector<Task<Result<int>>> ts;
            ts.push_back(succeed_task(10));
            ts.push_back(fail_task());
            ts.push_back(succeed_task(30));
            auto res = co_await when_all(std::move(ts));
            if (res.size() != 3 || !res[0] || *res[0] != 10 || res[1] || !res[2] || *res[2] != 30)
                co_return Err(err("when_all mismatch"));
            co_return Ok(0);
        }());
        TK_CHECK(ctx, r.has_value());
    }

    // when_all：空输入产出空向量。
    {
        auto r = rt->block_on([]() -> Task<Result<int>> {
            std::vector<Task<Result<int>>> ts;
            auto res = co_await when_all(std::move(ts));
            co_return res.empty() ? Ok(0) : Err(err("nonempty"));
        }());
        TK_CHECK(ctx, r.has_value());
    }

    // C2 / M2：多核 EventLoop 移动 + 拆解（exercise Executor::create 的移动路径与退出前排空）。
    {
        auto rt2 = runtime::Runtime::builder().worker_threads(3).enable_blocking(false).build();
        TK_CHECK(ctx, rt2.has_value());
        if (rt2) {
            auto rr = rt2->run([](async::EventLoop&, usize) -> Result<Unit> { return Ok(); });
            TK_CHECK(ctx, rr.has_value());
            rt2->stop_and_join();
        }
    }

    return ctx.summary();
}
