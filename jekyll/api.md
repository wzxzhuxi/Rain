---
title: API 文档
description: Rain 最常用的运行时、协程、网络与阻塞桥接入口概览
permalink: /api/
---

<section class="content-page" markdown="1">

# API 文档

本文档只覆盖 Rain 最常用的公开入口，重点解释运行时、协程、网络与阻塞桥接的使用面。

## 基础类型：`rain::Result` / `rain::Unit`

| API | 作用 |
| --- | --- |
| `Result<T, E>` | 基于 `std::expected` 的结果类型，默认错误类型为 `SystemError`。 |
| `Ok(value)` / `Ok()` | 构造成功结果。 |
| `Err(error)` | 构造失败结果。 |
| `Unit` / `unit` | 用于替代 `void` 的值类型，适合 `Result<Unit>` 和 `Task<Unit>`。 |
| `SystemError::from_errno()` | 从当前 `errno` 构造错误并带上源码位置。 |

## 协程任务：`rain::async::Task<T>` / `JoinHandle<T>`

| API | 作用 |
| --- | --- |
| `Task<T>` | 惰性协程封装，创建时不立即运行。 |
| `task.get()` | 同步跑完整个任务并取结果，主要用于测试。 |
| `task.request_stop()` | 请求协作式取消。 |
| `task.stop_token()` | 获取任务的 `stop_token`。 |
| `JoinHandle<T>` | 已 `spawn` 任务的非拥有观察者，用于等待结果或请求停止。 |
| `handle.done()` | 检查任务是否已经完成。 |
| `co_await handle` | 等待已投递任务完成，返回 `Result<T>`。 |

## 事件循环：`rain::async::EventLoop`

| API | 作用 |
| --- | --- |
| `EventLoop::create()` | 创建单个事件循环，内部初始化 reactor、timer 与跨核唤醒 fd。 |
| `loop.spawn(task)` | 在当前 loop 上投递即发即弃任务。 |
| `loop.spawn_with_handle(task)` | 投递任务并返回 `JoinHandle`。 |
| `loop.submit(task)` | 线程安全地把任务提交到目标 loop，用于跨核投递。 |
| `loop.run()` | 进入事件循环主循环，直到请求停止或所有任务完成。 |
| `loop.run_until_complete(task)` | 把单个主任务投递进去并运行到完成。 |
| `loop.request_stop()` | 请求 loop 停止。 |
| `loop.add_signals({...})` | 配置 `signalfd` 信号集合。 |
| `loop.wait_signal()` | 等待一个信号事件。 |
| `loop.reactor()` / `loop.timer()` | 访问当前 loop 绑定的 reactor 与 timer。 |

## 组合子：`rain::async`

| API | 作用 |
| --- | --- |
| `and_then(f)` | 对 `Task<Result<T>>` 成功值做链式异步转换。 |
| `map(f)` | 对成功值做同步映射。 |
| `map_err(f)` | 对错误做映射。 |
| `or_else(f)` | 失败时走恢复路径。 |
| `inspect(f)` / `inspect_err(f)` | 成功或失败旁路观察，不改变值。 |
| `when_all(tasks)` | 并发等待一组任务全部完成。 |
| `when_any(tasks)` | 任一任务成功即可返回，并取消其他任务。 |
| `retry(make_task, times, delay)` | 带延迟的重试组合。 |
| `with_deadline_check(task, timeout)` | 事后检查超时预算是否超出。 |

## 运行时：`rain::runtime::Runtime::Builder`

| API | 作用 |
| --- | --- |
| `.worker_threads(n)` | 设置 EventLoop 工作线程数。 |
| `.blocking_threads(n)` | 设置阻塞线程池线程数。 |
| `.blocking_queue_capacity(n)` | 设置阻塞线程池队列容量。 |
| `.enable_blocking(enable)` | 打开或关闭阻塞线程池。 |
| `.build()` | 组装 `Runtime`，返回 `Result<Runtime>`。 |

## 运行时入口：`rain::runtime::Runtime`

| API | 作用 |
| --- | --- |
| `Runtime::builder()` | 创建构建器。 |
| `rt.block_on(task)` | 在当前线程创建临时 `EventLoop` 并同步运行一个任务。 |
| `rt.run(setup)` | 启动多核运行时，在每个 core 上执行一次 setup。 |
| `rt.executor()` | 访问内部 `Executor`。 |
| `rt.thread_pool()` | 访问内部 `ThreadPool`。 |
| `rt.has_thread_pool()` | 判断当前 Runtime 是否启用了阻塞池。 |
| `rt.stop()` / `rt.join()` / `rt.stop_and_join()` | 停止或等待运行时退出。 |

## 多核编排：`rain::runtime::Executor`

| API | 作用 |
| --- | --- |
| `Executor::create(n)` | 创建 `n` 个 EventLoop，但不立刻启动线程。 |
| `executor.on_thread_init(fn)` | 为每个 worker 线程注册初始化回调。 |
| `executor.on_teardown(fn)` | 为每个 worker 线程注册退出回调。 |
| `executor.run(setup)` | 启动全部 worker，并在每个 core 上执行 setup。 |
| `executor.loop_at(index)` | 访问指定 core 的 EventLoop。 |
| `executor.core_count()` | 返回核心数。 |

## 阻塞桥接：`rain::runtime::ThreadPool` / `spawn_blocking`

| API | 作用 |
| --- | --- |
| `ThreadPool(num_threads, queue_capacity)` | 创建阻塞线程池。 |
| `pool.submit(fn, args...)` | 提交带返回值任务，返回 `future`。 |
| `pool.spawn(fn)` | 即发即弃提交。 |
| `pool.shutdown()` | 关闭队列并等待全部 worker 退出。 |
| `runtime::spawn_blocking(fn)` | 在协程内把阻塞工作卸载到线程池，并在完成后恢复到当前 EventLoop。 |
| `runtime::spawn(task)` | 在当前线程局部 EventLoop 上投递任务。 |
| `runtime::spawn_with_handle(task)` | 投递任务并返回 `JoinHandle`。 |
| `runtime::spawn_on(target, task)` | 把任务提交到指定 EventLoop。 |

## 网络：`rain::net::TcpListener` / `rain::net::TcpStream`

| API | 作用 |
| --- | --- |
| `TcpListener::bind(reactor, addr, opts)` | 绑定监听 socket。 |
| `listener.accept()` | 异步接收连接，返回 `TcpStream + SocketAddress`。 |
| `TcpStream::connect(reactor, addr)` | 主动建立非阻塞 TCP 连接。 |
| `stream.read(buf, len)` | 异步读取。 |
| `stream.write(buf, len)` | 异步写入一段数据。 |
| `stream.write_all(buf, len)` | 异步写完整个缓冲区。 |
| `stream.send_file_all_with_timeout(...)` | 在超时预算内发送完整文件区间。 |
| `stream.set_nodelay(enable)` | 配置 `TCP_NODELAY`。 |
| `stream.shutdown()` / `stream.close()` | 关闭连接。 |

## 线程局部上下文

Rain 在运行中的 worker 线程上自动安装这些 thread-local 指针：

| API | 作用 |
| --- | --- |
| `rain::async::g_reactor` | 当前核心的 reactor。 |
| `rain::async::g_timer` | 当前核心的时间轮。 |
| `rain::async::g_event_loop` | 当前核心的 EventLoop。 |
| `rain::runtime::g_thread_pool` | 当前线程可用的阻塞线程池。 |

这些指针的目的是简化框架内部和协程内的零参数调用；它们依赖运行时生命周期，不适合脱离 `EventLoop` / `Runtime` 手动长期持有。

</section>
