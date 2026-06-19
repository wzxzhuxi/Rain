// Channel 测试：sync 层的跨线程阻塞队列。
// 覆盖容量/背压（M1 的 try_send 路径）、关闭语义、drain-on-close，以及多生产者多消费者。

#include "sync/channel.hpp"
#include "testkit.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace rain;

auto main() -> int
{
    rain::test::Context ctx("channel");

    // 有界容量：try_send 填满后返回 Err，try_recv 腾出空间后又可发。
    {
        sync::Channel<int> ch(2);
        TK_CHECK(ctx, ch.try_send(1).has_value());
        TK_CHECK(ctx, ch.try_send(2).has_value());
        TK_CHECK(ctx, !ch.try_send(3).has_value()); // 满
        TK_CHECK_EQ(ctx, ch.size(), usize { 2 });
        auto v = ch.try_recv();
        TK_CHECK(ctx, v.has_value() && *v == 1);
        TK_CHECK(ctx, ch.try_send(3).has_value()); // 有空位了
    }

    // 关闭语义：关闭后 send 失败；空且关闭时 recv 返回 nullopt。
    {
        sync::Channel<int> ch(4);
        (void)ch.close();
        TK_CHECK(ctx, ch.is_closed());
        TK_CHECK(ctx, !ch.try_send(1).has_value());
        TK_CHECK(ctx, !ch.try_recv().has_value());
    }

    // drain-on-close：关闭前已入队的元素仍能被 recv 取出，取空后才返回 nullopt。
    {
        sync::Channel<int> ch(4);
        TK_CHECK(ctx, ch.try_send(10).has_value());
        TK_CHECK(ctx, ch.try_send(20).has_value());
        (void)ch.close();
        auto a = ch.try_recv();
        auto b = ch.try_recv();
        TK_CHECK(ctx, a && *a == 10 && b && *b == 20);
        TK_CHECK(ctx, !ch.try_recv().has_value());
    }

    // 多生产者多消费者：阻塞 send/recv 在普通线程上（非事件循环线程），不丢消息。
    {
        constexpr int kProducers = 4;
        constexpr int kPerProducer = 1000;
        sync::Channel<int> ch(64);
        std::atomic<long> sum { 0 };
        std::atomic<int> received { 0 };

        std::vector<std::jthread> consumers;
        for (int c = 0; c < 2; ++c) {
            consumers.emplace_back([&] {
                while (auto v = ch.recv()) {
                    sum.fetch_add(*v);
                    received.fetch_add(1);
                }
            });
        }
        std::vector<std::jthread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&] {
                for (int i = 0; i < kPerProducer; ++i)
                    (void)ch.send(1);
            });
        }
        for (auto& p : producers)
            p.join();
        (void)ch.close(); // 唤醒消费者退出
        for (auto& c : consumers)
            c.join();

        TK_CHECK_EQ(ctx, received.load(), kProducers * kPerProducer);
        TK_CHECK_EQ(ctx, sum.load(), static_cast<long>(kProducers) * kPerProducer);
    }

    return ctx.summary();
}
