#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/event_loop.hpp"
#include "async/task.hpp"
#include "runtime/bridge.hpp"
#include "runtime/executor.hpp"
#include "runtime/thread_pool.hpp"

#include <memory>
#include <optional>

namespace rain::runtime {
// 每工作线程的阻塞池指针；Runtime 启动时安装，全局 spawn_blocking 据此取池。
inline thread_local ThreadPool* g_thread_pool = nullptr;

namespace detail {
// 在事件循环回收协程帧前把结果搬到调用方栈上，避免 block_on 读已销毁的 promise。
template<typename T>
inline auto capture_into(std::optional<Result<T>>& out, async::JoinHandle<T> handle) -> async::Task<Unit>
{
    out = co_await std::move(handle);
    co_return unit;
}
} // namespace detail

// 统一持有 Executor（异步）与 ThreadPool（阻塞），提供 block_on 与多核 run 两个入口。
class Runtime {
public:
    class Builder;
    Runtime(const Runtime&) = delete;
    auto operator=(const Runtime&) -> Runtime& = delete;
    Runtime(Runtime&&) = default;
    auto operator=(Runtime&&) -> Runtime& = delete;
    [[nodiscard]] static auto builder() -> Builder;

    // 当前线程建临时 EventLoop，把 task 跑完。
    template<typename T>
    [[nodiscard]] auto block_on(async::Task<T> task) -> T
    {
        auto loop_result = async::EventLoop::create();
        if (!loop_result) {
            if constexpr (async::detail::is_result<T>::value)
                return T(Err(std::move(loop_result).error()));
            else
                std::abort(); // 非 Result 任务无错误通道
        }

        auto& loop = *loop_result;
        auto handle = loop.spawn_with_handle(std::move(task));
        std::optional<Result<T>> slot;
        loop.spawn(detail::capture_into(slot, std::move(handle)));
        g_thread_pool = pool_ ? &*pool_ : nullptr;
        (void)loop.run();
        g_thread_pool = nullptr;
        auto result = std::move(slot).value_or(Err(async::make_cancelled_error()));
        if constexpr (async::detail::is_result<T>::value) {
            if (!result)
                return T(Err(std::move(result).error()));
            return std::move(*result);
        } else {
            if (!result)
                std::abort(); // 未完成/被取消，非 Result 任务无值可返
            return std::move(*result);
        }
    }

    // 启动全部工作线程；setup 在每核调用一次，且每线程自动安装 g_thread_pool。
    [[nodiscard]] auto run(Executor::SetupFn setup) -> Result<Unit>
    {
        ThreadPool* pool_ptr = pool_ ? &*pool_ : nullptr;
        executor_.on_thread_init([pool_ptr](usize) { g_thread_pool = pool_ptr; });
        return executor_.run(std::move(setup));
    }

    [[nodiscard]] auto executor() -> Executor& { return executor_; }
    [[nodiscard]] auto executor() const -> const Executor& { return executor_; }
    [[nodiscard]] auto thread_pool() -> ThreadPool& { return *pool_; }
    [[nodiscard]] auto has_thread_pool() const noexcept -> bool { return pool_ != nullptr; }
    [[nodiscard]] auto core_count() const noexcept -> usize { return executor_.core_count(); }

    auto stop() -> Unit { return executor_.stop(); }
    auto request_graceful_stop() -> Unit { return executor_.request_graceful_stop(); }
    [[nodiscard]] auto graceful_stop(async::Duration deadline) -> Result<Unit>
    {
        return executor_.graceful_stop(deadline);
    }
    [[nodiscard]] auto join() -> Result<Unit> { return executor_.join(); }

    auto stop_and_join() -> Unit
    {
        stop();
        (void)join();
        return unit;
    }

private:
    Runtime(Executor executor, Box<ThreadPool> pool) : executor_(std::move(executor)), pool_(std::move(pool)) { }

    Executor executor_;
    Box<ThreadPool> pool_;
};

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
        if (!executor)
            return Err(std::move(executor).error());
        Box<ThreadPool> pool;
        if (enable_blocking_)
            pool = std::make_unique<ThreadPool>(blockers_, queue_cap_);
        return Ok(Runtime(std::move(*executor), std::move(pool)));
    }

private:
    usize workers_ = Executor::default_core_count();
    usize blockers_ = Executor::default_core_count();
    usize queue_cap_ = kDefaultTaskQueueCapacity;
    bool enable_blocking_ = true;
};

inline auto Runtime::builder() -> Runtime::Builder { return Builder { }; }

// 读取线程局部的全局便捷函数，可在任意协程内直接调用。
template<typename T>
auto spawn(async::Task<T> task) -> Unit
{
    if (async::g_event_loop)
        async::g_event_loop->spawn(std::move(task));
    return unit;
}

template<typename T>
[[nodiscard]] auto spawn_with_handle(async::Task<T> task) -> async::JoinHandle<T>
{
    return async::g_event_loop->spawn_with_handle(std::move(task));
}

template<typename F>
    requires std::invocable<F>
[[nodiscard]] auto spawn_blocking(F&& fn) -> async::Task<
    Result<std::conditional_t<std::is_void_v<std::invoke_result_t<F>>, Unit, std::invoke_result_t<F>>>>
{
    return ::rain::runtime::spawn_blocking(*async::g_reactor, *g_thread_pool, std::forward<F>(fn));
}

template<typename T>
[[nodiscard]] auto spawn_on(async::EventLoop& target, async::Task<T> task) -> Result<Unit>
{
    return target.submit(std::move(task));
}
} // namespace rain::runtime
