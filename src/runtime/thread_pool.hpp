#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "sync/channel.hpp"

#include <atomic>
#include <concepts>
#include <functional>
#include <future>
#include <thread>
#include <vector>

namespace rain::runtime {
static constexpr usize kDefaultTaskQueueCapacity = 1024;
static constexpr usize kFallbackThreadCount = 1;
using ExceptionHandler = std::move_only_function<void(std::exception_ptr)>;

inline auto default_exception_handler(std::exception_ptr eptr) -> void
{
    try {
        std::rethrow_exception(eptr);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[ThreadPool] unhandled exception: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "[ThreadPool] unhandled unknown exception\n");
    }
}

// 面向阻塞/CPU 任务的线程池，内部用 sync::Channel 分发任务。
// 用 future/packaged_task 是因为它天然支持带返回值的提交；异常由 on_error_ 兜底。
class ThreadPool {
public:
    explicit ThreadPool(usize num_threads = default_thread_count(),
                        usize queue_capacity = kDefaultTaskQueueCapacity,
                        ExceptionHandler on_error = default_exception_handler)
        : tasks_(queue_capacity), on_error_(std::move(on_error))
    {
        workers_.reserve(num_threads);
        for (usize i = 0; i < num_threads; ++i)
            workers_.emplace_back([this] { worker_loop(); });
    }

    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool&) = delete;
    auto operator=(const ThreadPool&) -> ThreadPool& = delete;
    ThreadPool(ThreadPool&&) = delete;
    auto operator=(ThreadPool&&) -> ThreadPool& = delete;

    // 通过 future 提交任务——调用方需要返回值时使用。
    template<typename F, typename... Args>
        requires std::invocable<F, Args...>
    [[nodiscard]] auto submit(F&& f, Args&&... args) -> Result<std::future<std::invoke_result_t<F, Args...>>>
    {
        using ReturnType = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto future = task->get_future();
        auto send_result = tasks_.send([task = std::move(task)]() mutable { (*task)(); });
        if (!send_result)
            return Err(std::move(send_result).error());
        return Ok(std::move(future));
    }

    // 即发即弃：非阻塞入队；队列满时返回 Err（调用方常在事件循环线程上，不能阻塞）。
    template<typename F>
        requires std::invocable<F>
    [[nodiscard]] auto spawn(F&& f) -> Result<Unit>
    {
        return tasks_.try_send(std::forward<F>(f));
    }

    auto shutdown() -> Unit
    {
        if (!shutdown_.exchange(true)) {
            tasks_.close();
            for (auto& worker : workers_)
                if (worker.joinable())
                    worker.join();
        }
        return unit;
    }

    [[nodiscard]] auto size() const noexcept -> usize { return workers_.size(); }
    [[nodiscard]] auto pending() const -> usize { return tasks_.size(); }
    [[nodiscard]] auto is_shutdown() const noexcept -> bool { return shutdown_.load(); }

    [[nodiscard]] static auto default_thread_count() noexcept -> usize
    {
        const auto hw = std::thread::hardware_concurrency();
        return hw > 0 ? static_cast<usize>(hw) : kFallbackThreadCount;
    }

private:
    auto worker_loop() -> Unit
    {
        while (auto task = tasks_.recv()) {
            try {
                (*task)();
            } catch (...) {
                on_error_(std::current_exception());
            }
        }
        return unit;
    }

    sync::Channel<std::move_only_function<void()>> tasks_;
    ExceptionHandler on_error_;
    std::vector<std::jthread> workers_;
    std::atomic<bool> shutdown_ { false };
};

[[nodiscard]] inline auto make_thread_pool(usize num_threads = ThreadPool::default_thread_count(),
                                           usize queue_capacity = kDefaultTaskQueueCapacity) -> ThreadPool
{
    return ThreadPool(num_threads, queue_capacity);
}
} // namespace rain::runtime
