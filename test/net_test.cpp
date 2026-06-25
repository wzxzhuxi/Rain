// net/ 行为测试——填补"网络运行时零网络测试"的 blocker。
// 分层确定性:纯 I/O(read/write_all/部分读/EOF)用 socketpair+from_fd（真零端口）；
// accept/connect 往返用 ephemeral 端口 0 + local_address() 取回内核分配端口。
// 单 EventLoop + when_all 同核并发驱动，贴每核 shared-nothing 模型、无跨线程 TSan 噪声。
// 真正的 teeth 是 ASan/TSan：注册/唤醒/帧回收的 lifetime/race 会直接暴露。

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/combinators.hpp"
#include "net/tcp_listener.hpp"
#include "net/tcp_stream.hpp"
#include "runtime/runtime.hpp"

#include "testkit.hpp"

#include <array>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace rain;
using async::Task;
using net::SocketAddress;
using net::TcpListener;
using net::TcpStream;
using rain::test::Context;

namespace {

// ── accept/connect 往返：server accept→read，client connect→write_all ──
[[nodiscard]] auto accept_and_read(TcpListener listener, Shared<usize> got, Shared<u8> first) -> Task<Result<Unit>>
{
    auto accepted = co_await listener.accept();
    if (!accepted)
        co_return Err(std::move(accepted).error());
    auto [stream, peer] = std::move(*accepted);
    std::array<u8, 16> buf {};
    auto n = co_await stream.read(buf.data(), buf.size());
    if (!n)
        co_return Err(std::move(n).error());
    *got = *n;
    *first = buf[0];
    co_return Ok();
}

[[nodiscard]] auto connect_and_write(async::EpollReactor& reactor, SocketAddress addr) -> Task<Result<Unit>>
{
    auto stream = co_await TcpStream::connect(reactor, addr);
    if (!stream)
        co_return Err(std::move(stream).error());
    const std::array<u8, 3> msg { 7, 8, 9 };
    auto w = co_await stream->write_all(msg.data(), msg.size());
    if (!w)
        co_return Err(std::move(w).error());
    co_return Ok();
}

} // namespace

auto main() -> int
{
    Context ctx("net");

    auto rt_result = runtime::Runtime::builder().worker_threads(1).enable_blocking(false).build();
    TK_CHECK(ctx, rt_result.has_value());
    if (!rt_result)
        return ctx.summary();
    auto rt = std::move(*rt_result);

    // case 1: socketpair write_all → read 往返（纯 I/O，无端口）
    {
        int sv[2] = { -1, -1 };
        TK_CHECK(ctx, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        const int s0 = sv[0], s1 = sv[1];
        auto r = rt.block_on([s0, s1]() -> Task<Result<Unit>> {
            auto a = TcpStream::from_fd(*async::g_reactor, s0);
            auto b = TcpStream::from_fd(*async::g_reactor, s1);
            if (!a || !b)
                co_return Err(SystemError::from_errno());
            const std::array<u8, 5> msg { 10, 20, 30, 40, 50 };
            auto w = co_await a->write_all(msg.data(), msg.size());
            if (!w)
                co_return Err(std::move(w).error());
            std::array<u8, 16> buf {};
            auto n = co_await b->read(buf.data(), buf.size());
            if (!n)
                co_return Err(std::move(n).error());
            if (*n != 5 || buf[0] != 10 || buf[4] != 50)
                co_return Err(SystemError { .code = std::make_error_code(std::errc::io_error),
                                            .message = "roundtrip mismatch",
                                            .location = std::source_location::current() });
            co_return Ok();
        }());
        TK_CHECK(ctx, r.has_value());
    }

    // case 2: 部分读——write 100 字节，read 进 16 字节缓冲 → 返回 ≤16（read_some 语义）
    {
        int sv[2] = { -1, -1 };
        TK_CHECK(ctx, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        const int s0 = sv[0], s1 = sv[1];
        auto r = rt.block_on([s0, s1]() -> Task<Result<usize>> {
            auto a = TcpStream::from_fd(*async::g_reactor, s0);
            auto b = TcpStream::from_fd(*async::g_reactor, s1);
            if (!a || !b)
                co_return Err(SystemError::from_errno());
            std::array<u8, 100> big {};
            auto w = co_await a->write_all(big.data(), big.size());
            if (!w)
                co_return Err(std::move(w).error());
            std::array<u8, 16> buf {};
            co_return co_await b->read(buf.data(), buf.size());
        }());
        TK_CHECK(ctx, r.has_value());
        if (r)
            TK_CHECK(ctx, *r >= 1 && *r <= 16); // 部分读：不会超过缓冲大小
    }

    // case 3: EOF——一端 shutdown(SHUT_WR)，另一端 read 得 0
    {
        int sv[2] = { -1, -1 };
        TK_CHECK(ctx, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        const int s0 = sv[0], s1 = sv[1];
        auto r = rt.block_on([s0, s1]() -> Task<Result<usize>> {
            auto a = TcpStream::from_fd(*async::g_reactor, s0);
            auto b = TcpStream::from_fd(*async::g_reactor, s1);
            if (!a || !b)
                co_return Err(SystemError::from_errno());
            (void)a->shutdown(SHUT_WR); // a 半关 → b 读到 EOF
            std::array<u8, 16> buf {};
            co_return co_await b->read(buf.data(), buf.size());
        }());
        TK_CHECK(ctx, r.has_value());
        if (r)
            TK_CHECK_EQ(ctx, *r, usize { 0 }); // EOF
    }

    // case 4: accept/connect 往返（ephemeral 端口 0 + local_address）
    {
        auto r = rt.block_on([]() -> Task<Result<Unit>> {
            auto& reactor = *async::g_reactor;
            auto listener = TcpListener::bind(reactor, SocketAddress::loopback(0));
            if (!listener)
                co_return Err(std::move(listener).error());
            auto addr = listener->local_address(); // 取回内核分配的 ephemeral 端口
            if (!addr)
                co_return Err(std::move(addr).error());

            auto got = std::make_shared<usize>(0);
            auto first = std::make_shared<u8>(0);
            std::vector<Task<Result<Unit>>> racers;
            racers.push_back(accept_and_read(std::move(*listener), got, first));
            racers.push_back(connect_and_write(reactor, *addr));
            auto results = co_await async::when_all(std::move(racers));

            for (auto& res : results)
                if (!res)
                    co_return Err(std::move(res).error());
            if (*got != 3 || *first != 7)
                co_return Err(SystemError { .code = std::make_error_code(std::errc::io_error),
                                            .message = "accept/connect roundtrip mismatch",
                                            .location = std::source_location::current() });
            co_return Ok();
        }());
        TK_CHECK(ctx, r.has_value());
    }

    return ctx.summary();
}
