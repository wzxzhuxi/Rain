<p align="center">
  <img src="jekyll/images/rain-logo-animated.gif" alt="Rain logo" width="112">
</p>

<h1 align="center">Rain</h1>

<p align="center"><strong>C++23 Async Runtime</strong></p>

<p align="center">Per-core EventLoop · Coroutine Tasks · epoll Reactor · Hierarchical Timer Wheel · ThreadPool · spawn_blocking bridge</p>

<p align="center">
  <a href="README.md">中文</a> ·
  <a href="#quick-start">Quick Start</a> ·
  <a href="#build">Build</a> ·
  <a href="#benchmark">Benchmark</a> ·
  <a href="LICENSE">MIT License</a>
</p>

<p align="center">
  <img alt="C++23" src="https://img.shields.io/badge/C%2B%2B-23-111827?style=flat-square">
  <img alt="Platform" src="https://img.shields.io/badge/Platform-Linux-2563eb?style=flat-square">
  <img alt="Backend" src="https://img.shields.io/badge/Backend-epoll-0f766e?style=flat-square">
  <img alt="License" src="https://img.shields.io/badge/License-MIT-16a34a?style=flat-square">
</p>

Documentation site: [Rain](https://wzxzhuxi.github.io/Rain/)

Rain is a Linux-oriented C++23 async runtime built around per-core event loops, coroutine tasks, an epoll reactor, a hierarchical timer wheel, a thread pool, and a sync-async bridge.

The goal is not to maximize features. The goal is to keep the runtime small enough that each component can be read and understood in isolation.

| Area | Summary |
| --- | --- |
| Execution model | per-core shared-nothing |
| Core abstractions | `Result<T, E>`, `Task<T>`, `EventLoop` |
| Blocking bridge | `ThreadPool + eventfd + spawn_blocking()` |
| Networking | `TcpListener`, `TcpStream`, I/O awaiters |

## Benchmark

TCP echo, `wrk -t4 -c256 -d15s`, both sides built in Release mode, measured on a Ryzen 5 5600H (6C/12T):

| Metric | Rain | Tokio |
| --- | ---: | ---: |
| Requests/sec | 247,283 | 219,543 |
| Avg Latency | 788 us | 819 us |
| Max Latency | 9.31 ms | 18.45 ms |
| Stdev | 282 us | 477 us |
| Timeouts | 0 | 0 |

Run it with:

```bash
cd rain
cmake -B build -DRAIN_BUILD_BENCHMARKS=ON
cmake --build build
bash benchmark/run_bench.sh
```

### Why is Rain even faster than Tokio?
Is Lao Wang a genius? Unfortunately, no.

Rain comes out ahead in this benchmark mainly because it does not use work stealing. That removes a chunk of the overhead caused by moving tasks across CPU cores.

Does that mean Rain has no application-level automatic load balancing? Correct. Rain does not try to balance work at the application layer. Instead, it leans on kernel-level load balancing.

For network workloads, `SO_REUSEPORT` lets the kernel distribute incoming connections across CPU cores automatically. That avoids much of the cost of automatic cross-core task transfer.

Does that mean Rain cannot hand work across cores at all? Also no. When cross-core handoff is needed, `EventLoop::submit()` can explicitly dispatch a task to a target core.

## Architecture

```text
src/
├── core/       type aliases, Result<T, E>, concepts
├── sync/       Channel, SpinLock
├── async/      Task, EventLoop, EpollReactor, timer wheel, signal, combinators
├── runtime/    Runtime, Executor, ThreadPool, spawn_blocking bridge
└── net/        TcpListener, TcpStream, address, I/O awaiters
```

Layer dependencies are strictly top-down:

```text
Layer 0  core/       Foundation: type aliases, Result, concepts
Layer 1  sync/       Primitives: Channel, SpinLock
Layer 2  async/      Runtime core: Task, EventLoop, epoll, timers, signals
Layer 3  runtime/    Orchestration: Runtime, Executor, ThreadPool, Bridge
Layer 4  net/        Networking: TCP listener, stream, and I/O awaiters
```

## Quick Start

```cpp
#include "async/event_loop.hpp"
#include "async/task.hpp"
#include "core/types.hpp"
#include "net/address.hpp"
#include "net/tcp_listener.hpp"
#include "net/tcp_stream.hpp"
#include "runtime/runtime.hpp"

#include <array>
#include <memory>

using namespace rain;

namespace {

static constexpr char kResponse[] = "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: text/plain; charset=utf-8\r\n"
                                    "Content-Length: 13\r\n"
                                    "Connection: keep-alive\r\n"
                                    "\r\n"
                                    "Hello, World!";

[[nodiscard]] auto handle_conn(net::TcpStream stream) -> async::Task<Unit>
{
    std::array<u8, 128> buf{};

    while (true) {
        auto n = co_await stream.read(buf.data(), buf.size());
        if (!n || *n == 0) {
            co_return unit;
        }

        auto w = co_await stream.write_all(
            reinterpret_cast<const u8*>(kResponse), sizeof(kResponse) - 1);
        if (!w) {
            co_return unit;
        }
    }
}

[[nodiscard]] auto accept_loop(Shared<net::TcpListener> listener) -> async::Task<Unit>
{
    while (true) {
        auto accepted = co_await listener->accept();
        if (!accepted) {
            continue;
        }

        auto [stream, _] = std::move(*accepted);
        runtime::spawn(handle_conn(std::move(stream)));
    }

    co_return unit;
}

} // namespace

auto main() -> int
{
    auto rt = runtime::Runtime::builder()
        .worker_threads(4)
        .enable_blocking(false)
        .build();
    if (!rt) {
        return 1;
    }

    auto run_result = rt->run([](async::EventLoop& loop, usize) -> Result<Unit> {
        auto listener = net::TcpListener::bind(loop.reactor(), net::SocketAddress::any(9000));
        if (!listener) {
            return Err(std::move(listener).error());
        }

        auto shared = std::make_shared<net::TcpListener>(std::move(*listener));
        loop.spawn(accept_loop(shared));
        return Ok();
    });

    if (!run_result) {
        return 1;
    }

    rt->join();
    return 0;
}
```

## Build

```bash
cd rain
cmake -B build
cmake --build build
```

Enable benchmark targets:

```bash
cd rain
cmake -B build -DRAIN_BUILD_BENCHMARKS=ON
cmake --build build
```

## Requirements

- C++23 (GCC 13+ or Clang 17+)
- Linux (`epoll`, `eventfd`, `signalfd`)
- CMake 3.28+

## Key Design Points

- Per-core shared-nothing: each core owns its own `EventLoop`, reactor, and timer.
- `Result<T, E>` everywhere: no exceptions as control flow.
- Lazy coroutines with symmetric transfer: `Task<T>` starts suspended and resumes through continuation chaining.
- Hierarchical timer wheel: four-level O(1)-style timer management.
- `spawn_blocking`: blocking work is offloaded through `ThreadPool + eventfd` and resumed on the original event loop.
- Cross-core submission: `EventLoop::submit()` lets the caller explicitly push work to another core.

## Components

| Component | File | Purpose |
| --- | --- | --- |
| `Task<T>` | `src/async/task.hpp` | Lazy coroutine, symmetric transfer, cooperative cancellation |
| `EventLoop` | `src/async/event_loop.hpp` | Per-core scheduler: ready queue + epoll + timer + submit |
| `EpollReactor` | `src/async/io_reactor.hpp` | epoll wrapper and I/O awaiter support |
| `HierarchicalTimerWheel` | `src/async/timer.hpp` | Hierarchical timer wheel |
| `Executor` | `src/runtime/executor.hpp` | Multi-core EventLoop orchestration |
| `ThreadPool` | `src/runtime/thread_pool.hpp` | Blocking task execution pool |
| `Bridge` | `src/runtime/bridge.hpp` | Sync-async bridge behind `spawn_blocking()` |
| `Runtime` | `src/runtime/runtime.hpp` | Unified runtime entry |
| `TcpListener` | `src/net/tcp_listener.hpp` | Async accept |
| `TcpStream` | `src/net/tcp_stream.hpp` | Async read/write and sendfile |

## License

Rain is released under the [MIT License](LICENSE).

## Acknowledgements and References

### Projects

- [Nginx](https://github.com/nginx/nginx): reference for the per-worker shared-nothing model and `SO_REUSEPORT` usage.
- [Tokio](https://github.com/tokio-rs/tokio): reference for per-core runtime layout, `spawn_blocking`, and timer design.
- [libuv](https://github.com/libuv/libuv): reference for event loop and reactor design.
- [zedio](https://github.com/8sileus/zedio): reference for modern C++ async runtime design.
- [TinyWebServer](https://github.com/qinguoyi/TinyWebServer): reference for Linux networking and epoll practice.
- [Linux kernel](https://kernel.org): reference for the hierarchical timer wheel algorithm.

### Paper

- [Hashed and Hierarchical Timing Wheels](http://www.cs.columbia.edu/~nahum/w6998/papers/ton97-timing-wheels.pdf): the classic timing wheel paper.

### Books

- *The Art of Unix Programming*
- *High-Performance Linux Server Programming*
- *Functional Programming in C++*
