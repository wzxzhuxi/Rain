// 生命周期隐患回归测试——评审点名但零覆盖的 teardown 路径。
// 这些断言当前（安全）行为，真正的 teeth 是 ASan/TSan：若 teardown 顺序错误导致协程帧/
// reactor/cross_core_queue 的 use-after-free，会直接变成进程级失败。
//
// 隐患 #2：run() 退出时 owned_tasks_ 仍有"挂在 reactor 上、永不就绪"的协程帧 → ~EventLoop
//          destroy 它（帧析构早于 reactor 析构，reactor dtor 不解引用悬空注册指针 → 安全）。
// 隐患 #1：跨核 submit 的任务在 worker 退出前/后留在 cross_core_queue_ → ~EventLoop 安全销毁。

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/event_loop.hpp"
#include "async/task.hpp"
#include "net/tcp_stream.hpp"
#include "runtime/bridge.hpp"
#include "runtime/runtime.hpp"
#include "runtime/thread_pool.hpp"

#include "testkit.hpp"

#include <array>
#include <atomic>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace rain;
using async::Task;
using rain::test::Context;

namespace {

// 永久阻塞的 reader（拥有 fd，帧析构时关闭它）；只有 teardown destroy 才结束。
[[nodiscard]] auto blocking_reader(int fd) -> Task<Unit>
{
    auto s = net::TcpStream::from_fd(*async::g_reactor, fd);
    if (!s)
        co_return unit;
    std::array<u8, 16> buf {};
    (void)co_await s->read(buf.data(), buf.size()); // 永久挂起，直到帧被 teardown 销毁
    co_return unit;
}

[[nodiscard]] auto noop() -> Task<Unit> { co_return unit; }

// 自由协程函数（避免 lambda 协程的捕获生命周期陷阱）：spawn_blocking 一个自旋 worker，
// 参数全是长生命周期对象的引用，协程帧存的是指针、不依赖任何临时闭包。
[[nodiscard]] auto blocking_spinner(async::EpollReactor& reactor, runtime::ThreadPool& pool,
    std::atomic<bool>& started, std::atomic<bool>& release, std::atomic<bool>& wrote) -> Task<Unit>
{
    (void)co_await runtime::spawn_blocking(reactor, pool, [&started, &release, &wrote] {
        started.store(true);
        while (!release.load()) { } // 自旋保持 in-flight
        wrote.store(true);
    });
    co_return unit;
}

} // namespace

auto main() -> int
{
    Context ctx("lifetime");

    // 隐患 #2：teardown 时存在挂起且仍注册在 reactor 上的协程帧
    {
        auto rt = runtime::Runtime::builder().worker_threads(2).enable_blocking(false).build();
        TK_CHECK(ctx, rt.has_value());
        if (rt) {
            std::vector<int> peers; // 对端 fd，测试末尾关闭
            auto rr = rt->run([&peers](async::EventLoop& loop, usize) -> Result<Unit> {
                int sv[2] = { -1, -1 };
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
                    peers.push_back(sv[1]);       // 永不写 → reader 永久阻塞
                    loop.spawn(blocking_reader(sv[0])); // 帧在 teardown 时仍挂起
                }
                return Ok();
            });
            TK_CHECK(ctx, rr.has_value());
            rt->stop_and_join(); // teardown：owned_tasks_ 非空（含挂起帧）
            for (int fd : peers)
                ::close(fd);
        }
    }

    // 隐患 #1：跨核 submit 的任务在 teardown 时留在 cross_core_queue_
    {
        auto rt = runtime::Runtime::builder().worker_threads(2).enable_blocking(false).build();
        TK_CHECK(ctx, rt.has_value());
        if (rt) {
            auto rr = rt->run([](async::EventLoop&, usize) -> Result<Unit> { return Ok(); });
            TK_CHECK(ctx, rr.has_value());
            // 从主线程跨核提交（任意线程可调 submit）
            auto sub = rt->executor().loop_at(0).submit(noop());
            TK_CHECK(ctx, sub.has_value());
            rt->stop_and_join(); // teardown：cross_core_queue_ 可能残留任务帧
        }
    }

    // 隐患 #3：spawn_blocking 帧被销毁后 worker 仍写回。用非 Runtime 的"不安全析构顺序"
    // (loop 先于 pool 销毁) 故意构造该窗口：worker 自旋保持 in-flight → 销毁 loop（帧被
    // destroy）→ 放行 worker。修复后 worker 写的是共享状态而非已销毁帧，ASan/TSan 安全。
    {
        runtime::ThreadPool pool(1, 16); // 外层：最后销毁（join worker）
        std::atomic<bool> started { false };
        std::atomic<bool> release { false };
        std::atomic<bool> wrote { false };
        {
            auto loop_r = async::EventLoop::create();
            TK_CHECK(ctx, loop_r.has_value());
            if (loop_r) {
                auto& loop = *loop_r;
                loop.spawn(blocking_spinner(loop.reactor(), pool, started, release, wrote));
                std::thread loop_thread([&loop] { (void)loop.run(); });
                while (!started.load()) { } // 等 worker 进入 → spawn_blocking 已挂起且 in-flight
                loop.request_stop();        // run() 在 worker 仍 in-flight 时退出（eventfd 即时唤醒）
                loop_thread.join();
            }
        } // loop 在此销毁 → 挂起的 spawn_blocking 帧被 destroy（worker 仍自旋）
        release.store(true); // 放行：worker 写共享状态（修复后安全）而非已销毁帧
        while (!wrote.load()) { }
        TK_CHECK(ctx, wrote.load());
    }

    return ctx.summary();
}
