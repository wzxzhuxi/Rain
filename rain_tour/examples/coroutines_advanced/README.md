# 协程进阶教程案例

中文 | [English](README_EN.md)

这个目录存放 `rain_07_coroutines_advanced` Manim 教程的配套代码案例：一个用 C++23、函数式风格写成的单线程 `epoll` + 协程 echo 服务器。

## 构建

```bash
make
```

## 案例说明

- `echo_server.cpp`：单线程 `epoll` + 协程 echo 服务器。`DetachedTask`（fire-and-forget 协程）+ `IoAwaiter`（注册 fd 进 epoll 并挂起）+ 事件循环（`epoll_wait` 后恢复协程）三件套，一个线程并发服务多连接——正是 Rain `Task`/`EpollReactor`/`EventLoop` 的缩影。

  **函数式要点**：用 `std::expected` 当 `Result<T, E>`（配 `Err()` 辅助），让可失败的初始化（`make_epoll`/`make_listener`）显式返回错误值、不抛异常——这正是 Rain `core/result.hpp` 里 `Ok`/`Err` 的同一套写法。地址构造 `make_addr`、事件构造 `make_event` 抽成纯函数；协程的 I/O 循环本质是副作用，保持命令式不强行扭曲。

## 运行

```bash
make run-echo       # 启动后另开终端：nc 127.0.0.1 9200，输入文字会被原样回显
```

这些案例面向 Linux，使用 `epoll`、非阻塞 socket 和 C++23 协程。
