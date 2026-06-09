# 线程教程案例

中文 | [English](README_EN.md)

这个目录存放 `rain_02_thread` Manim 教程的配套代码案例：一个用 C++23、函数式风格写成的通用作业线程池。

## 构建

```bash
make
```

## 案例说明

- `thread_pool_futures.cpp`：通用作业线程池。`submit` 用 `packaged_task` 返回 `std::future<R>`，调用方按需取回结果；析构走优雅排空——先跑完队列剩余作业再 join。对应 Rain runtime 中 ThreadPool 承接 `spawn_blocking` 的角色。

  **函数式要点**：线程池本身是命令式的「脏壳」（队列 + 互斥量 + 条件变量），但对外暴露的是函数式接口。`main` 里用纯函数 `square` 配合 ranges 管道声明式地驱动整个流程：`iota | transform | ranges::to` 建出 future 向量，`views::enumerate` 取结果，`ranges::fold_left` 求和——没有手写循环、没有可变累加器散落各处。

## 运行

```bash
make run-pool-futures
```

会派发 8 个平方作业到池中并发执行，按序取回结果并求和。

这些案例使用 C++23 标准库线程设施，编译时需要开启 pthread 支持。
