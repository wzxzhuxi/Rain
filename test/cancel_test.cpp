// 取消接线端到端验证：await_transform 传播 + 叶子 stop_callback + timeout 组合子。
// 用 socketpair 制造确定性 I/O：对端不写 → read 阻塞（可被抢占式取消）；对端先写 → read 立即完成。
// 真正的 teeth 是 ASan/TSan：取消路径的 deregister/notify/帧回收若有 UAF/竞争会直接暴露。

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/combinators.hpp"
#include "net/tcp_stream.hpp"
#include "runtime/runtime.hpp"

#include "testkit.hpp"

#include <array>
#include <chrono>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <vector>

using namespace rain;
using namespace std::chrono_literals;
using async::Task;
using rain::test::Context;

namespace {

// 永久阻塞的 reader（对端不写 → read 挂起，只有被取消才返回）
[[nodiscard]] auto blocking_reader(int fd) -> Task<Result<int>>
{
    auto s = net::TcpStream::from_fd(*async::g_reactor, fd);
    if (!s)
        co_return Err(std::move(s).error());
    std::array<u8, 16> buf {};
    auto n = co_await s->read(buf.data(), buf.size());
    if (!n)
        co_return Err(std::move(n).error());
    co_return Ok(static_cast<int>(*n));
}

[[nodiscard]] auto immediate_ok() -> Task<Result<int>> { co_return Ok(42); }

} // namespace

auto main() -> int
{
    Context ctx("cancel");

    auto rt_result = runtime::Runtime::builder().worker_threads(1).enable_blocking(false).build();
    TK_CHECK(ctx, rt_result.has_value());
    if (!rt_result)
        return ctx.summary();
    auto rt = std::move(*rt_result);

    // case 1：对端不写 → read 永远阻塞 → timeout 50ms 抢占式取消 → 返回 timed_out
    {
        int sv[2] = { -1, -1 };
        TK_CHECK(ctx, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        const int sv0 = sv[0], sv1 = sv[1];
        auto r = rt.block_on([sv0]() -> async::Task<Result<usize>> {
            auto stream = net::TcpStream::from_fd(*async::g_reactor, sv0);
            if (!stream)
                co_return Err(std::move(stream).error());
            std::array<u8, 16> buf {};
            co_return co_await async::timeout(stream->read(buf.data(), buf.size()), 50ms);
        }());
        TK_CHECK(ctx, !r.has_value()); // 应超时失败
        if (!r)
            TK_CHECK(ctx, r.error().code == std::errc::timed_out);
        ::close(sv1); // sv0 已被 TcpStream 析构关闭
    }

    // case 2：对端先写 → read 立即完成 → timeout 返回 Ok(2)，且不悬挂（5s 大超时下 loop 仍立即退出）
    {
        int sv[2] = { -1, -1 };
        TK_CHECK(ctx, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        const int sv0 = sv[0], sv1 = sv[1];
        const char msg[] = "hi";
        TK_CHECK(ctx, ::write(sv1, msg, 2) == 2);
        auto r = rt.block_on([sv0]() -> async::Task<Result<usize>> {
            auto stream = net::TcpStream::from_fd(*async::g_reactor, sv0);
            if (!stream)
                co_return Err(std::move(stream).error());
            std::array<u8, 16> buf {};
            co_return co_await async::timeout(stream->read(buf.data(), buf.size()), 5s);
        }());
        TK_CHECK(ctx, r.has_value());
        if (r)
            TK_CHECK_EQ(ctx, *r, usize { 2 });
        ::close(sv1);
    }

    // case 3: when_any 取消败者——永久阻塞 read 与立即成功者赛跑。立即成功者胜出后必须抢占式
    // 取消阻塞 read，否则 owned_tasks_ 永不空、block_on 挂死。测试能正常返回本身即验证取消生效。
    {
        int sv[2] = { -1, -1 };
        TK_CHECK(ctx, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        const int sv0 = sv[0], sv1 = sv[1];
        auto r = rt.block_on([sv0]() -> Task<Result<int>> {
            std::vector<Task<Result<int>>> racers;
            racers.push_back(blocking_reader(sv0)); // 永久阻塞（败者）
            racers.push_back(immediate_ok());       // 立即 Ok(42)（胜者）
            co_return co_await async::when_any(std::move(racers));
        }());
        TK_CHECK(ctx, r.has_value());
        if (r)
            TK_CHECK_EQ(ctx, *r, 42); // 立即成功者胜出
        ::close(sv1);
    }

    return ctx.summary();
}
