// 浸泡测试：高次反复敲打 spawn_blocking 的跨线程边界（事件循环线程 <-> 线程池 worker），
// 正是 bridge 把结果写回协程帧的 happens-before 所在。单发测试抓不到的罕见竞争，
// 在 TSan 下跑成千上万次才会稳定暴露。
//
// 迭代次数可由环境变量 RAIN_SOAK_ITERS 覆盖（CI 夜间任务调高）。

#include "async/combinators.hpp"
#include "runtime/runtime.hpp"

#include "testkit.hpp"

#include <cstdlib>
#include <vector>

using namespace rain;
using namespace rain::async;

static auto blocking_value(int v) -> Task<Result<int>>
{
    co_return co_await runtime::spawn_blocking([v] { return v; });
}

auto main() -> int
{
    rain::test::Context ctx("soak");

    int iters = 200;
    if (const char* e = std::getenv("RAIN_SOAK_ITERS")) {
        const int parsed = std::atoi(e);
        if (parsed > 0)
            iters = parsed;
    }
    constexpr int kFanout = 16;

    auto rt = runtime::Runtime::builder().worker_threads(1).enable_blocking(true).blocking_threads(4).build();
    TK_CHECK(ctx, rt.has_value());
    if (!rt)
        return ctx.summary();

    // 每轮并发派发 kFanout 个 spawn_blocking，when_all 汇合。总和应等于
    // iters * sum(0..kFanout-1) —— 任何丢任务/竞争都会让结果对不上或被 sanitizer 拦下。
    auto r = rt->block_on([iters]() -> Task<Result<long>> {
        long total = 0;
        for (int k = 0; k < iters; ++k) {
            std::vector<Task<Result<int>>> ts;
            ts.reserve(kFanout);
            for (int m = 0; m < kFanout; ++m)
                ts.push_back(blocking_value(m));
            auto res = co_await when_all(std::move(ts));
            for (auto& rr : res) {
                if (rr)
                    total += *rr;
            }
        }
        co_return Ok(total);
    }());

    const long expected = static_cast<long>(iters) * (kFanout * (kFanout - 1) / 2);
    TK_CHECK(ctx, r.has_value());
    if (r)
        TK_CHECK_EQ(ctx, *r, expected);

    return ctx.summary();
}
