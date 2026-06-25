// 优雅停机端到端验证：spawn_service 的 accept loop + 在途请求处理，graceful_stop 触发后
// accept loop 被抢占式取消而退出、在途请求 drain 跑完、run() 干净退出（非硬切）。
// 客户端用阻塞 socket 从主线程驱动，echo 往返确认请求确实被处理。

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/event_loop.hpp"
#include "async/task.hpp"
#include "net/tcp_listener.hpp"
#include "net/tcp_stream.hpp"
#include "runtime/runtime.hpp"

#include "testkit.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace rain;
using async::Task;
using net::SocketAddress;
using net::TcpListener;
using net::TcpStream;
using rain::test::Context;

namespace {

// 在途请求处理：读一次 → 计数 → echo 回去。完成即 drain。
[[nodiscard]] auto handle_conn(TcpStream stream, std::atomic<int>& handled) -> Task<Unit>
{
    std::array<u8, 16> buf {};
    auto n = co_await stream.read(buf.data(), buf.size());
    if (n && *n > 0) {
        handled.fetch_add(1, std::memory_order_relaxed);
        (void)co_await stream.write_all(buf.data(), *n);
    }
    co_return unit;
}

// 服务循环（spawn_service 注入优雅停机令牌）：优雅停机时挂起的 accept 被取消 → 循环退出。
[[nodiscard]] auto accept_loop(TcpListener listener, std::atomic<int>& handled) -> Task<Unit>
{
    while (true) {
        auto conn = co_await listener.accept();
        if (!conn)
            co_return unit; // 被优雅停机取消（或出错）→ 退出服务循环
        auto [stream, peer] = std::move(*conn);
        if (auto* loop = async::g_event_loop)
            loop->spawn(handle_conn(std::move(stream), handled));
    }
}

} // namespace

auto main() -> int
{
    Context ctx("graceful");

    auto rt_result = runtime::Runtime::builder().worker_threads(1).enable_blocking(false).build();
    TK_CHECK(ctx, rt_result.has_value());
    if (!rt_result)
        return ctx.summary();
    auto rt = std::move(*rt_result);

    std::atomic<int> handled { 0 };
    std::atomic<u16> port { 0 };

    // setup 在主线程同步跑（run() 返回前）：bind ephemeral + spawn_service(accept loop)
    auto rr = rt.run([&handled, &port](async::EventLoop& loop, usize) -> Result<Unit> {
        auto listener = TcpListener::bind(loop.reactor(), SocketAddress::loopback(0));
        if (!listener)
            return Err(std::move(listener).error());
        auto addr = listener->local_address();
        if (!addr)
            return Err(std::move(addr).error());
        port.store(addr->port(), std::memory_order_release);
        loop.spawn_service(accept_loop(std::move(*listener), handled));
        return Ok();
    });
    TK_CHECK(ctx, rr.has_value());

    // 客户端往返（阻塞 socket）：connect → write → read echo（确认请求被处理）
    {
        const int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
        TK_CHECK(ctx, cfd >= 0);
        struct sockaddr_in sa { };
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port.load(std::memory_order_acquire));
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        TK_CHECK(ctx, ::connect(cfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0);
        const char msg[] = "hi";
        TK_CHECK(ctx, ::write(cfd, msg, 2) == 2);
        char rbuf[16] = {};
        const auto rn = ::read(cfd, rbuf, sizeof(rbuf)); // 阻塞直到 handler echo → 同步点
        TK_CHECK(ctx, rn == 2);
        ::close(cfd);
    }

    // 优雅停机：accept loop 被取消而退出，在途 drain，run() 干净退出（500ms deadline 兜底）
    auto gr = rt.graceful_stop(std::chrono::milliseconds(500));
    TK_CHECK(ctx, gr.has_value());            // 干净退出，无 core 故障
    TK_CHECK(ctx, handled.load() == 1);       // 请求被处理（drain 完成）

    return ctx.summary();
}
