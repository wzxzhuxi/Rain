# 进程教程案例

中文 | [English](README_EN.md)

这个目录存放 `rain_01_process` Manim 教程的配套代码案例：一个用 C++23、函数式风格写成的崩溃容错 supervisor。

## 构建

```bash
make
```

## 案例说明

- `crash_supervisor.cpp`：崩溃容错 supervisor。父进程 fork 若干 worker，其中一个会故意在第 3 拍解引用空指针段错误；supervisor 在 `waitpid` 循环里检测到子进程被信号杀死（`WIFSIGNALED`），立刻重启一个健康 worker 顶替它——其余 worker 全程毫发无损。

  这演示了进程独有、线程/协程给不了的**崩溃边界**，也是 Nginx master/worker、systemd、Erlang/OTP supervisor 的根基。

  **函数式要点**：把「解释 wait status」(`classify`) 和「定位槽位」(`find_slot`) 抽成纯函数——输入定输出、可脱离进程单测；`fork` / `waitpid` / 打印等副作用留在 `main` 的边缘，是「core 纯、shell 脏」的分层。崩溃与否用 `std::optional` 表达，取代魔数 `-1`。

## 运行

```bash
make run-supervisor
```

会打印交错的 worker 心跳：你能看到某个 worker 段错误崩溃、被换上一个新 pid 的健康 worker，而其余 worker 的心跳从头到尾没有中断——这就是进程隔离的崩溃边界。

反过来这也解释了 Rain 的取舍：Rain 用线程 + 协程跑每核 EventLoop，放弃了这层崩溃隔离，换来共享地址空间和零 IPC 成本的性能。

> 实现细节：SIGALRM 用 `sigaction` 安装且**不设** `SA_RESTART`，保证它能打断阻塞的 `waitpid`（返回 `EINTR`）从而优雅收工。`std::signal` 在 glibc 上默认带 `SA_RESTART`，会自动重启 `waitpid` 导致永远退不出循环——这是个真实的坑。

面向 Linux，使用 `fork`、`waitpid`、`sigaction`、`setrlimit` 等 POSIX API。
