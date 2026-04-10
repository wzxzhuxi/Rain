---
title: 子库说明
description: Rain 五层模块职责、依赖方向与源码切入点
permalink: /libraries/
---

<section class="content-page" markdown="1">

# 子库说明

## 分层方向

```text
core -> sync -> async -> runtime -> net
```

原则：低层不依赖高层；运行时编排位于 `runtime/`；网络层只建立在运行时和异步能力之上。

## 模块清单

### `core`

基础类型、`Result<T, E>`、`SystemError`、概念约束。整个项目的最底层公共基础。

### `sync`

提供 `Channel` 和 `SpinLock` 这类同步原语，服务于线程池、跨线程提交和轻量同步场景。

### `async`

Rain 的核心：`Task`、`EventLoop`、`EpollReactor`、时间轮、信号、组合子都在这里。

### `runtime`

负责把 `Executor`、`ThreadPool` 和 `spawn_blocking` 桥接成完整运行时，对外暴露 `Runtime`。

### `net`

负责 `SocketAddress`、`TcpListener`、`TcpStream` 和各种 I/O awaiter，是网络传输层。

## 组件对照

| 组件 | 文件 | 作用 |
| --- | --- | --- |
| `Task<T>` | `src/async/task.hpp` | 惰性协程、对称转移、协作式取消 |
| `EventLoop` | `src/async/event_loop.hpp` | 每核调度器：ready queue + epoll + timer + submit |
| `EpollReactor` | `src/async/io_reactor.hpp` | 边缘触发 + oneshot epoll 封装 |
| `HierarchicalTimerWheel` | `src/async/timer.hpp` | O(1) 插入/取消的时间轮 |
| `Executor` | `src/runtime/executor.hpp` | 多核 EventLoop 编排 |
| `ThreadPool` | `src/runtime/thread_pool.hpp` | 基于 Channel 的阻塞任务线程池 |
| `Bridge` | `src/runtime/bridge.hpp` | `spawn_blocking` 同步-异步桥接 |
| `Runtime` | `src/runtime/runtime.hpp` | 统一入口：Executor + ThreadPool |
| `TcpListener` | `src/net/tcp_listener.hpp` | 异步 accept |
| `TcpStream` | `src/net/tcp_stream.hpp` | 异步读写与 sendfile |

## 阅读建议

1. 先从 `core` 和 `async/task.hpp` 入手，理解返回值和协程模型。
2. 然后阅读 `io_reactor.hpp`、`event_loop.hpp`、`timer.hpp`，建立单核执行模型。
3. 接着看 `executor.hpp` 与 `runtime.hpp`，理解多核编排和阻塞桥接。
4. 最后再读 `net/`，因为网络层只是把前面这些运行时能力投到 TCP 场景里。

</section>
