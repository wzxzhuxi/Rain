// 基于属性的测试：用固定种子的 RNG 生成大量随机输入，断言不变量。
// 无第三方依赖（不引 rapidcheck）——种子固定即可复现，契合极简风格。

#include "async/timer.hpp"
#include "sync/channel.hpp"
#include "testkit.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

using namespace rain;
using namespace rain::async;
using namespace std::chrono;

// 时间轮属性：随机 delay + 随机取消子集后，每个未取消定时器都在其 deadline
// 的 [delay, delay+2] tick 内恰好触发一次；每个已取消的定时器绝不触发。
// 这是当年抓出 C3 的全相位仿真的随机化升级，同时覆盖 M3 的取消。
static auto timer_property(unsigned seed, rain::test::Context& ctx) -> void
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> delay_dist(1, 30000);
    std::bernoulli_distribution cancel_dist(0.3);

    constexpr int N = 64;
    HierarchicalTimerWheel w;
    std::vector<WaitState> st(N, WaitState::Pending); // 预分配，指针稳定（不增长）
    std::vector<int> delay(N), fire_at(N, -1);
    std::vector<bool> cancelled(N, false);
    std::vector<TimerId> ids(N);

    int maxd = 0;
    for (int i = 0; i < N; ++i) {
        delay[i] = delay_dist(rng);
        maxd = std::max(maxd, delay[i]);
        ids[i] = w.schedule_at(w.current_time() + milliseconds(delay[i]), {}, &st[i]);
    }
    for (int i = 0; i < N; ++i) {
        if (cancel_dist(rng)) {
            cancelled[i] = true;
            (void)w.cancel(ids[i]);
        }
    }

    for (int t = 1; t <= maxd + 5; ++t) {
        (void)w.tick();
        for (int i = 0; i < N; ++i) {
            if (fire_at[i] < 0 && st[i] != WaitState::Pending)
                fire_at[i] = t;
        }
    }

    for (int i = 0; i < N; ++i) {
        if (cancelled[i]) {
            TK_CHECK(ctx, fire_at[i] == -1);
        } else {
            TK_CHECK(ctx, fire_at[i] >= delay[i] && fire_at[i] <= delay[i] + 2);
        }
    }
}

// Channel 属性：随机生产者/消费者数、消息数、容量下，消息既不丢失也不重复
// （守恒：收到的条数与总和都等于发送量）。每个生产者发送的值都是 1。
static auto channel_property(unsigned seed, rain::test::Context& ctx) -> void
{
    std::mt19937 rng(seed);
    const int producers = 1 + static_cast<int>(rng() % 4);
    const int consumers = 1 + static_cast<int>(rng() % 3);
    const int per = 100 + static_cast<int>(rng() % 400);
    const usize cap = 1 + (rng() % 32);

    sync::Channel<int> ch(cap);
    std::atomic<long> sum { 0 };
    std::atomic<int> received { 0 };

    std::vector<std::jthread> cons;
    for (int c = 0; c < consumers; ++c) {
        cons.emplace_back([&] {
            while (auto v = ch.recv()) {
                sum.fetch_add(*v);
                received.fetch_add(1);
            }
        });
    }
    std::vector<std::jthread> prod;
    for (int p = 0; p < producers; ++p) {
        prod.emplace_back([&] {
            for (int i = 0; i < per; ++i)
                (void)ch.send(1);
        });
    }
    for (auto& p : prod)
        p.join();
    (void)ch.close();
    for (auto& c : cons)
        c.join();

    TK_CHECK_EQ(ctx, received.load(), producers * per);
    TK_CHECK_EQ(ctx, sum.load(), static_cast<long>(producers) * per);
}

auto main() -> int
{
    rain::test::Context ctx("property");

    for (unsigned seed = 1; seed <= 40; ++seed)
        timer_property(seed, ctx);
    for (unsigned seed = 1; seed <= 12; ++seed)
        channel_property(seed, ctx);

    return ctx.summary();
}
