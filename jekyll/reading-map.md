---
title: 阅读地图
description: Rain 的源码入口、模块顺序和关键系统调用地图
permalink: /reading-map/
---

<section class="content-page" markdown="1">

# 阅读地图

## 整体顺序

阅读 Rain 更适合按源码层级展开：

1. `core`：类型系统、`Result`、错误模型
2. `sync`：`Channel`、`SpinLock`
3. `async`：`Task`、`EpollReactor`、`EventLoop`、时间轮、信号、组合子
4. `runtime`：`Executor`、`ThreadPool`、`Bridge`、`Runtime`
5. `net`：地址、监听器、TCP 流与 I/O awaiter

## 模块阅读顺序

### 第一轮：核心抽象

- `src/core/result.hpp`
- `src/core/types.hpp`
- `src/async/task.hpp`

### 第二轮：单核异步执行

- `src/async/io_reactor.hpp`
- `src/async/timer.hpp`
- `src/async/signal.hpp`
- `src/async/event_loop.hpp`

### 第三轮：运行时编排

- `src/runtime/thread_pool.hpp`
- `src/runtime/bridge.hpp`
- `src/runtime/executor.hpp`
- `src/runtime/runtime.hpp`

### 第四轮：网络层

- `src/net/address.hpp`
- `src/net/tcp_listener.hpp`
- `src/net/tcp_stream.hpp`
- `src/net/io_awaiters.hpp`

## 关键系统调用

| 系统调用 | Rain 组件 | 用途 |
| --- | --- | --- |
| `epoll_create` / `epoll_ctl` / `epoll_wait` | `async/io_reactor.hpp` | I/O 多路复用 |
| `read` / `write` | `net/stream_io_ops.hpp` | 非阻塞 TCP 收发 |
| `socket` / `bind` / `listen` / `accept` | `net/` | TCP 生命周期 |
| `eventfd` | `runtime/bridge.hpp`、`async/event_loop.hpp` | 跨线程与跨核唤醒 |
| `signalfd` | `async/signal.hpp` | 把信号整合进 epoll |
| `sendfile` | `net/stream_io_ops.hpp` | 零拷贝文件发送 |
| `sigaction` | `runtime/executor.hpp` | 忽略 `SIGPIPE` |

## 源码阅读入口

| 入口文件 | 适合解决的问题 |
| --- | --- |
| `src/core/result.hpp` | Rain 如何表达错误与成功 |
| `src/async/task.hpp` | 协程值如何传播、如何连接 continuation |
| `src/async/io_reactor.hpp` | reactor 如何登记 fd 与恢复协程 |
| `src/async/event_loop.hpp` | 每核 loop 如何调度 ready / timer / epoll |
| `src/runtime/thread_pool.hpp` | 阻塞任务如何排队和执行 |
| `src/runtime/bridge.hpp` | `spawn_blocking` 如何通过 eventfd 唤醒 loop |
| `src/runtime/runtime.hpp` | 统一运行时如何装配 |
| `src/net/tcp_stream.hpp` | TCP 异步 I/O 如何落到 awaiter |

## 建议的实际阅读方式

1. 先从 `Task` 和 `Result` 入手，再进入 `EventLoop` / `Reactor`。
2. 遇到跨线程与阻塞桥接，再看 `ThreadPool` 和 `Bridge`。
3. 最后用 `benchmark/echo_bench.cpp` 把运行时入口和网络路径串起来。

</section>
