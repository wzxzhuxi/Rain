#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/event_loop.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <signal.h>
#include <thread>
#include <vector>

namespace rain::runtime {
// 每核 EventLoop 编排器：N 个循环运行在独立线程上，管理生命周期与故障策略。
class Executor {
public:
    using SetupFn = std::move_only_function<Result<Unit>(async::EventLoop&, usize core_id)>;
    using ThreadInitFn = std::function<void(usize core_id)>;
    using TeardownFn = std::function<void(usize core_id)>;

    enum class FaultPolicy {
        FailFast, // 一核失败即停所有核（默认）
        LogOnly,  // 仅上报并计数，其余核继续
    };

    auto fault_policy(FaultPolicy p) -> Executor&
    {
        policy_ = p;
        return *this;
    }

    Executor(const Executor&) = delete;
    auto operator=(const Executor&) -> Executor& = delete;
    Executor(Executor&&) = default;
    auto operator=(Executor&&) -> Executor& = delete;

    ~Executor() { stop_and_join(); }

    static constexpr usize kFallbackCoreCount = 1;

    [[nodiscard]] static auto default_core_count() noexcept -> usize
    {
        const auto hw = std::thread::hardware_concurrency();
        return hw > 0 ? static_cast<usize>(hw) : kFallbackCoreCount;
    }

    [[nodiscard]] static auto create(usize num_cores = default_core_count()) -> Result<Executor>
    {
        std::vector<async::EventLoop> loops;
        loops.reserve(num_cores);
        for (usize i = 0; i < num_cores; ++i) {
            auto loop = async::EventLoop::create();
            if (!loop)
                return Err(std::move(loop).error());
            loops.push_back(std::move(*loop));
        }
        return Ok(Executor(std::move(loops)));
    }

    auto on_thread_init(ThreadInitFn fn) -> Executor&
    {
        thread_init_ = std::move(fn);
        return *this;
    }

    auto on_teardown(TeardownFn fn) -> Executor&
    {
        teardown_ = std::move(fn);
        return *this;
    }

    // 先在主线程跑完所有 setup 再统一 spawn worker：某核 setup 失败时不会有 worker 已在运行。
    [[nodiscard]] auto run(SetupFn setup) -> Result<Unit>
    {
        if (auto sigpipe = ignore_sigpipe(); !sigpipe)
            return Err(std::move(sigpipe).error());
        threads_.reserve(loops_.size());
        for (usize i = 0; i < loops_.size(); ++i) {
            auto setup_result = setup(loops_[i], i);
            if (!setup_result) {
                stop_and_join();
                return Err(std::move(setup_result).error());
            }
        }

        for (usize i = 0; i < loops_.size(); ++i) {
            threads_.emplace_back([this, i] {
                if (thread_init_)
                    thread_init_(i);
                auto run_result = loops_[i].run();
                if (!run_result) {
                    {
                        std::lock_guard lk(*faults_mu_);
                        faults_.push_back(std::move(run_result).error());
                    }
                    if (policy_ == FaultPolicy::FailFast) {
                        for (auto& loop : loops_)
                            loop.request_stop();
                    }
                }

                if (teardown_)
                    teardown_(i);
            });
        }
        return Ok();
    }

    // 硬停：在途任务被直接撕断。
    auto stop() -> Unit
    {
        for (auto& loop : loops_)
            loop.request_stop();
        return unit;
    }

    // 停 accept、服务循环退出；在途任务继续 drain。
    auto request_graceful_stop() -> Unit
    {
        for (auto& loop : loops_)
            loop.request_graceful_stop();
        return unit;
    }

    // deadline 内未 drain 完则硬停残留；返回各核 run() 故障（若有）。
    [[nodiscard]] auto graceful_stop(async::Duration deadline) -> Result<Unit>
    {
        request_graceful_stop();
        std::atomic<bool> drained { false };
        std::thread watchdog([this, deadline, &drained] {
            const auto end = std::chrono::steady_clock::now() + deadline;
            while (std::chrono::steady_clock::now() < end) {
                if (drained.load(std::memory_order_acquire))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            stop();
        });
        auto result = join();
        drained.store(true, std::memory_order_release);
        watchdog.join();
        return result;
    }

    // 返回首个 core 故障，让退出码成为类型契约。
    [[nodiscard]] auto join() -> Result<Unit>
    {
        for (auto& t : threads_)
            if (t.joinable())
                t.join();
        threads_.clear();

        // 被移动后的残壳 faults_mu_ 为 null（析构会走到这里）。
        if (faults_mu_) {
            std::lock_guard lk(*faults_mu_);
            if (!faults_.empty())
                return Err(std::move(faults_.front()));
        }
        return Ok();
    }

    auto stop_and_join() -> Unit
    {
        stop();
        (void)join();
        return unit;
    }

    [[nodiscard]] auto core_count() const noexcept -> usize { return loops_.size(); }

    [[nodiscard]] auto loop_at(usize index) -> async::EventLoop& { return loops_[index]; }

private:
    explicit Executor(std::vector<async::EventLoop> loops) : loops_(std::move(loops)) { }

    [[nodiscard]] static auto ignore_sigpipe() -> Result<Unit>
    {
        if (::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
            return Err(SystemError::from_errno());
        return Ok();
    }

    std::vector<async::EventLoop> loops_;
    std::vector<std::jthread> threads_;
    ThreadInitFn thread_init_;
    TeardownFn teardown_;
    Box<std::mutex> faults_mu_ = std::make_unique<std::mutex>(); // mutex 不可移动，装箱保 Executor 可移动
    std::vector<SystemError> faults_;
    FaultPolicy policy_ = FaultPolicy::FailFast;
};
} // namespace rain::runtime
