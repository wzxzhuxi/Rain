// Rain TCP Echo Benchmark
//
// 编译:
//   g++ -std=c++23 -O2 -I src -o rain/benchmark/echo_bench rain/benchmark/echo_bench.cpp -lpthread
//
// 压测:
//   ./rain/benchmark/echo_bench
//   wrk -t4 -c100 -d10s http://127.0.0.1:7778/

#include "async/event_loop.hpp"
#include "async/task.hpp"
#include "core/types.hpp"
#include "net/address.hpp"
#include "net/tcp_listener.hpp"
#include "net/tcp_stream.hpp"
#include "runtime/runtime.hpp"

#include <array>
#include <cstdio>
#include <memory>

using namespace rain;

namespace {

static constexpr char kResponse[] = "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: text/html; charset=utf-8\r\n"
                                    "Content-Length: 13\r\n"
                                    "Connection: keep-alive\r\n"
                                    "\r\n"
                                    "Hello, World!";

static constexpr u16 kPort = 7778;

[[nodiscard]] auto handle_conn(net::TcpStream stream) -> async::Task<Unit>
{
    std::array<u8, 128> buf{};

    while (true) {
        auto n = co_await stream.read(buf.data(), buf.size());
        if (!n || *n == 0)
            co_return unit;

        auto w = co_await stream.write_all(
            reinterpret_cast<const u8*>(kResponse), sizeof(kResponse) - 1);
        if (!w)
            co_return unit;
    }
}

[[nodiscard]] auto accept_loop(Shared<net::TcpListener> listener) -> async::Task<Unit>
{
    while (true) {
        auto accepted = co_await listener->accept();
        if (!accepted)
            continue;

        auto [stream, _] = std::move(*accepted);
        (void)stream.set_nodelay(true);

        if (auto* loop = async::g_event_loop) {
            loop->spawn(handle_conn(std::move(stream)));
        }
    }

    co_return unit;
}

} // namespace

auto main() -> int
{
    constexpr usize cores = 4;

    auto rt = runtime::Runtime::builder()
        .worker_threads(cores)
        .enable_blocking(false)
        .build();

    if (!rt) {
        std::fprintf(stderr, "failed to create runtime\n");
        return 1;
    }

    const auto bind_addr = net::SocketAddress::any(kPort);
    const net::ListenOptions listen_opts{
        .backlog = 4096,
        .reuse_addr = true,
        .reuse_port = true,
    };

    auto run_result = rt->run([&](async::EventLoop& loop, usize) -> Result<Unit> {
        auto listener = net::TcpListener::bind(loop.reactor(), bind_addr, listen_opts);
        if (!listener)
            return Err(std::move(listener).error());

        auto shared = std::make_shared<net::TcpListener>(std::move(*listener));
        loop.spawn(accept_loop(shared));
        return Ok();
    });

    if (!run_result) {
        std::fprintf(stderr, "failed to start: %s\n", run_result.error().message.c_str());
        return 2;
    }

    std::printf("rain echo benchmark :%d (%zu cores)\n", kPort, cores);
    rt->join();
    return 0;
}
