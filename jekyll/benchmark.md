---
title: 基准测试
description: Rain 当前 echo benchmark 的口径、结果与对照代码
permalink: /benchmark/
---

<section class="content-page" markdown="1">

# Benchmark

<div class="doc-callout">
  <p class="doc-callout-title">Results</p>
  <p>这些结果来自 Rain 仓库当前 README 中记录的一组本机测试，不代表跨机器的绝对结论。比较时请保证编译模式、核心数和 wrk 参数一致。</p>
</div>

## Server configuration

- CPU：AMD Ryzen 5 5600H (6C / 12T)
- Build：双方均为 Release
- Workload：TCP echo
- Rain 端口：`7778`
- Tokio 端口：`7779`

## Test tool

[wrk](https://github.com/wg/wrk)

```bash
wrk -t4 -c256 -d15s http://127.0.0.1:<port>/
```

仓库内置一键脚本：

```bash
cd rain
bash benchmark/run_bench.sh
```

## Result

| 指标 | Rain | Tokio |
| --- | ---: | ---: |
| Requests/sec | 247,283 | 219,543 |
| Avg Latency | 788 us | 819 us |
| Max Latency | 9.31 ms | 18.45 ms |
| Stdev | 282 us | 477 us |
| Timeouts | 0 | 0 |

## Rain benchmark source

<details class="bench-details" markdown="1">
<summary>rain/benchmark/echo_bench.cpp</summary>

```cpp
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
```

</details>

## Tokio benchmark source

<details class="bench-details" markdown="1">
<summary>rain/benchmark/tokio_echo/src/main.rs</summary>

```rust
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;

const RESPONSE: &[u8] = b"HTTP/1.1 200 OK\r\n\
Content-Type: text/html; charset=utf-8\r\n\
Content-Length: 13\r\n\
Connection: keep-alive\r\n\
\r\n\
Hello, World!";

const PORT: u16 = 7779;

async fn handle_conn(mut stream: tokio::net::TcpStream) {
    let mut buf = [0u8; 128];
    loop {
        let n = match stream.read(&mut buf).await {
            Ok(0) | Err(_) => return,
            Ok(n) => n,
        };
        let _ = n;
        if stream.write_all(RESPONSE).await.is_err() {
            return;
        }
    }
}

#[tokio::main]
async fn main() {
    let listener = TcpListener::bind(format!("0.0.0.0:{PORT}"))
        .await
        .expect("bind failed");

    println!("tokio echo benchmark :{PORT}");

    loop {
        let (stream, _) = match listener.accept().await {
            Ok(conn) => conn,
            Err(_) => continue,
        };
        stream.set_nodelay(true).ok();
        tokio::spawn(handle_conn(stream));
    }
}
```

</details>

</section>
