---
title: 安装与使用
description: Rain 的构建、最小启动、benchmark 与本地文档预览方式
permalink: /install-and-usage/
---

<section class="content-page" markdown="1">

# 安装与使用

## 环境要求

- GCC 13+ 或 Clang 17+
- CMake 3.28+
- GNU Make 或 Ninja
- Linux（当前运行时依赖 `epoll`、`eventfd`、`signalfd`）

## 构建

最小构建：

```bash
cd rain
cmake -B build
cmake --build build
```

编译检查目标：

```bash
cd rain
./build/rain_compile_check
```

启用 benchmark：

```bash
cd rain
cmake -B build -DRAIN_BUILD_BENCHMARKS=ON
cmake --build build
```

启用测试：

```bash
cd rain
cmake -B build -DRAIN_BUILD_TESTS=ON
cmake --build build
```

## 最小入口

```cpp
#include "runtime/runtime.hpp"
#include "net/tcp_listener.hpp"

using namespace rain;

[[nodiscard]] auto handle_conn(net::TcpStream stream) -> async::Task<Unit>
{
    std::array<u8, 128> buf{};
    static constexpr char kResponse[] = "HTTP/1.1 200 OK\r\n"
                                        "Content-Length: 13\r\n"
                                        "Connection: keep-alive\r\n"
                                        "\r\n"
                                        "Hello, World!";

    while (true) {
        auto n = co_await stream.read(buf.data(), buf.size());
        if (!n || *n == 0) {
            co_return unit;
        }
        auto w = co_await stream.write_all(reinterpret_cast<const u8*>(kResponse), sizeof(kResponse) - 1);
        if (!w) {
            co_return unit;
        }
    }
}

[[nodiscard]] auto accept_loop(Shared<net::TcpListener> listener) -> async::Task<Unit>
{
    while (auto accepted = co_await listener->accept()) {
        auto [stream, _] = std::move(*accepted);
        runtime::spawn(handle_conn(std::move(stream)));
    }
    co_return unit;
}

auto main() -> int
{
    auto rt = runtime::Runtime::builder()
        .worker_threads(4)
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

## 压测

Rain 自带最短 echo benchmark：

```bash
cd rain
cmake -B build -DRAIN_BUILD_BENCHMARKS=ON
cmake --build build
./build/rain_echo_bench
wrk -t4 -c100 -d10s http://127.0.0.1:7778/
```

一键运行 Rain vs Tokio：

```bash
cd rain
bash benchmark/run_bench.sh
```

## 本地启动文档站

```bash
cd rain/jekyll
bundle install
bundle exec jekyll serve --livereload
```

默认地址：`http://127.0.0.1:4000`

</section>
