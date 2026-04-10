#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/event_loop.hpp"
#include "async/task.hpp"
#include "runtime/bridge.hpp"
#include "runtime/executor.hpp"
#include "runtime/thread_pool.hpp"

#include <memory>

namespace rain::runtime {

// ── 线程局部：阻塞线程池指针 ─────────────────────────────────────────
// 由 Runtime 在每个工作线程启动时安装，使全局 spawn_blocking
// 无需手动传递 ThreadPool 引用。

inline thread_local ThreadPool* g_thread_pool = nullptr;

// ── 运行时（Runtime）────────────────────────────────────────────────
// 通用异步运行时底座。统一持有 Executor（异步任务）和 ThreadPool
// （阻塞任务），提供 block_on（单线程入口）和 run（多核入口）。
//
// 用法：
//   auto rt = Runtime::builder()
//       .worker_threads(4)
//       .blocking_threads(8)
//       .build();
//
//   // 单线程入口
//   auto result = rt->block_on(my_async_task());
//
//   // 多核入口
//   rt->run([](async::EventLoop& loop, usize) -> Result<Unit> {
//       loop.spawn(my_task());
//       return Ok();
//   });
//   rt->executor().join();

class Runtime {
public:
    class Builder;

    Runtime(const Runtime&) = delete;
    auto operator=(const Runtime&) -> Runtime& = delete;
    Runtime(Runtime&&) = default;
    auto operator=(Runtime&&) -> Runtime& = delete;

    [[nodiscard]] static auto builder() -> Builder;

    // ── 单线程同步入口 ─────────────────────────────────────────────
    // 在当前线程创建临时 EventLoop，运行 task 直到完成。
    // 适用于 main()、测试、简单脚本。

    template<typename T>
    [[nodiscard]] auto block_on(async::Task<T> task) -> T
    {
        auto loop_result = async::EventLoop::create();
        if (!loop_result) {
            if constexpr (std::same_as<T, Result<Unit>>) {
                return Err(std::move(loop_result).error());
            } else {
                std::abort();
            }
        }

        auto& loop = *loop_result;
        auto handle = loop.spawn_with_handle(std::move(task));

        g_thread_pool = pool_ ? &*pool_ : nullptr;
        (void)loop.run();
        g_thread_pool = nullptr;

        if constexpr (std::same_as<T, Result<Unit>>) {
            return handle.await_resume();
        } else {
            auto result = handle.await_resume();
            return std::move(*result);
        }
    }

    // ── 多核入口 ───────────────────────────────────────────────────
    // 启动 Executor 全部工作线程。setup 在每核上调用一次。
    // 每个工作线程自动安装 g_thread_pool。

    [[nodiscard]] auto run(Executor::SetupFn setup) -> Result<Unit>
    {
        ThreadPool* pool_ptr = pool_ ? &*pool_ : nullptr;

        executor_.on_thread_init([pool_ptr](usize) {
            g_thread_pool = pool_ptr;
        });

        return executor_.run(std::move(setup));
    }

    // ── 访问内部组件 ─────────────────────────────────────────────

    [[nodiscard]] auto executor() -> Executor& { return executor_; }
    [[nodiscard]] auto executor() const -> const Executor& { return executor_; }

    [[nodiscard]] auto thread_pool() -> ThreadPool&
    {
        return *pool_;
    }

    [[nodiscard]] auto has_thread_pool() const noexcept -> bool { return pool_ != nullptr; }

    [[nodiscard]] auto core_count() const noexcept -> usize { return executor_.core_count(); }

    auto stop() -> Unit { return executor_.stop(); }

    auto join() -> Unit { return executor_.join(); }

    auto stop_and_join() -> Unit
    {
        stop();
        join();
        return unit;
    }

private:
    Runtime(Executor executor, Box<ThreadPool> pool)
        : executor_(std::move(executor)), pool_(std::move(pool))
    { }

    Executor executor_;
    Box<ThreadPool> pool_;
};

// ── 构建器（Runtime::Builder）───────────────────────────────────────

class Runtime::Builder {
public:
    auto worker_threads(usize n) -> Builder&
    {
        workers_ = n;
        return *this;
    }

    auto blocking_threads(usize n) -> Builder&
    {
        blockers_ = n;
        return *this;
    }

    auto blocking_queue_capacity(usize n) -> Builder&
    {
        queue_cap_ = n;
        return *this;
    }

    auto enable_blocking(bool enable) -> Builder&
    {
        enable_blocking_ = enable;
        return *this;
    }

    [[nodiscard]] auto build() -> Result<Runtime>
    {
        auto executor = Executor::create(workers_);
        if (!executor) {
            return Err(std::move(executor).error());
        }

        Box<ThreadPool> pool;
        if (enable_blocking_) {
            pool = std::make_unique<ThreadPool>(blockers_, queue_cap_);
        }

        return Ok(Runtime(std::move(*executor), std::move(pool)));
    }

private:
    usize workers_ = Executor::default_core_count();
    usize blockers_ = Executor::default_core_count();
    usize queue_cap_ = kDefaultTaskQueueCapacity;
    bool enable_blocking_ = true;
};

inline auto Runtime::builder() -> Runtime::Builder { return Builder { }; }

// ── 全局便捷函数 ────────────────────────────────────────────────────
// 读取 thread-local，在任何协程内可直接调用。

template<typename T>
auto spawn(async::Task<T> task) -> Unit
{
    if (async::g_event_loop) {
        async::g_event_loop->spawn(std::move(task));
    }
    return unit;
}

template<typename T>
[[nodiscard]] auto spawn_with_handle(async::Task<T> task) -> async::JoinHandle<T>
{
    return async::g_event_loop->spawn_with_handle(std::move(task));
}

template<typename F>
    requires std::invocable<F>
[[nodiscard]] auto spawn_blocking(F&& fn) -> async::Task<Result<std::invoke_result_t<F>>>
{
    return ::rain::runtime::spawn_blocking(*async::g_reactor, *g_thread_pool, std::forward<F>(fn));
}

template<typename T>
[[nodiscard]] auto spawn_on(async::EventLoop& target, async::Task<T> task) -> Result<Unit>
{
    return target.submit(std::move(task));
}

} // namespace rain::runtime
