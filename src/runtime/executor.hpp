#pragma once

#include "core/diagnostics.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include "async/event_loop.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <signal.h>
#include <thread>
#include <vector>

namespace rain::runtime {

// ── 执行器（Executor）────────────────────────────────────────────────
// 每核 EventLoop 编排器。创建 N 个 EventLoop，并各自运行在独立线程上，
// 提供生命周期管理（start/stop/join）。
//
// 这是多核服务运行的入口：
//   auto executor = Executor::create(4);
//   executor->run([](async::EventLoop& loop, usize) -> Result<Unit> {
//       loop.spawn(my_accept_task(loop));
//       return Ok();
//   });
//   executor->join();
//
// 每个线程拥有独立的 EventLoop、epoll、定时器与信号处理。
// 跨线程通信通过 CrossThreadNotifier + Channel（见 bridge.hpp）完成。

class Executor {
public:
    using SetupFn = std::move_only_function<Result<Unit>(async::EventLoop&, usize core_id)>;
    using ThreadInitFn = std::function<void(usize core_id)>;
    using TeardownFn = std::function<void(usize core_id)>;

    // core 失败策略：一个 worker 的 run() 返回 Err 时怎么办。
    enum class FaultPolicy {
        FailFast, // 一核死即对所有 loop request_stop，让整个 Executor 停机（默认）
        LogOnly,  // 仅经 diag seam 上报并计数，其余核继续
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

    // 工厂方法：创建 N 个 EventLoop，但暂不启动线程。
    [[nodiscard]] static auto create(usize num_cores = default_core_count()) -> Result<Executor>
    {
        std::vector<async::EventLoop> loops;
        loops.reserve(num_cores);

        for (usize i = 0; i < num_cores; ++i) {
            auto loop = async::EventLoop::create();
            if (!loop) {
                return Err(std::move(loop).error());
            }
            loops.push_back(std::move(*loop));
        }

        return Ok(Executor(std::move(loops)));
    }

    // 注册每核线程初始化回调。在工作线程中 loop.run() 之前调用。
    // 用于安装线程局部状态（如 g_thread_pool）。必须在 run() 前设置。
    auto on_thread_init(ThreadInitFn fn) -> Executor&
    {
        thread_init_ = std::move(fn);
        return *this;
    }

    // 注册每核清理回调。在线程中 loop.run() 返回后调用。
    // 必须在 run() 前设置。
    auto on_teardown(TeardownFn fn) -> Executor&
    {
        teardown_ = std::move(fn);
        return *this;
    }

    // 启动全部 EventLoop 线程。setup 会在每个核心上调用一次，
    // 且发生在 loop 正式运行之前，可用于投递初始任务
    // （如 accept 循环、信号处理等）。
    //
    // setup(loop, core_id) 返回 Result<Unit>。
    // 任一核心 setup 失败时，会停止所有循环并返回错误。
    [[nodiscard]] auto run(SetupFn setup) -> Result<Unit>
    {
        auto sigpipe_result = Executor::ignore_sigpipe();
        if (!sigpipe_result) {
            return Err(std::move(sigpipe_result).error());
        }

        threads_.reserve(loops_.size());

        // 先在主线程跑完所有 setup，再统一 spawn worker。setup 中的 add_signals
        // 会在主线程屏蔽信号，而 worker 在创建时继承主线程掩码——全部 setup 先行
        // 可确保每个 worker 都继承到所有已屏蔽信号，进程定向信号不会落到未屏蔽的
        // worker 上绕过 signalfd（M4）。同时也避免某核 setup 失败时已有 worker 在跑。
        for (usize i = 0; i < loops_.size(); ++i) {
            auto setup_result = setup(loops_[i], i);
            if (!setup_result) {
                stop_and_join();
                return Err(std::move(setup_result).error());
            }
        }

        // 每核诊断计数器：deque（CoreMetrics 含 atomic 不可移动，不能用会重分配的 vector）。
        metrics_.resize(loops_.size());

        for (usize i = 0; i < loops_.size(); ++i) {
            threads_.emplace_back([this, i] {
                diag::g_metrics = &metrics_[i];
                diag::MetricsRegistry::instance().add(&metrics_[i]);
                if (thread_init_)
                    thread_init_(i);

                auto run_result = loops_[i].run();
                if (!run_result) {
                    diag::count(&diag::CoreMetrics::loop_run_errors);
                    diag::emit({ .code = "loop.run_failed",
                                 .severity = diag::Severity::Error,
                                 .error = &run_result.error() });
                    {
                        std::lock_guard lk(*faults_mu_);
                        faults_.push_back(std::move(run_result).error());
                    }
                    if (policy_ == FaultPolicy::FailFast) {
                        // 静默死核 = 它的 SO_REUSEPORT accept 队列没人收 → 停掉所有核（request_stop 跨核安全）
                        for (auto& l : loops_)
                            l.request_stop();
                    }
                }

                if (teardown_)
                    teardown_(i);
                diag::MetricsRegistry::instance().remove(&metrics_[i]);
                diag::g_metrics = nullptr;
            });
        }

        return Ok();
    }

    // 请求所有 EventLoop 停止（非阻塞，硬停——在途任务被直接撕断）。
    auto stop() -> Unit
    {
        for (auto& loop : loops_) {
            loop.request_stop();
        }
        return unit;
    }

    // 请求所有 EventLoop 优雅停机（非阻塞）：停 accept、服务循环退出，在途任务继续 drain。
    auto request_graceful_stop() -> Unit
    {
        for (auto& loop : loops_) {
            loop.request_graceful_stop();
        }
        return unit;
    }

    // 优雅停机 + 带 deadline 的兜底：停 accept → 等在途 drain；deadline 内未 drain 完则硬停残留。
    // 返回各核 run() 故障（若有）。watchdog 线程在 deadline 后硬停；drain 提前完成则立即收回 watchdog。
    [[nodiscard]] auto graceful_stop(async::Duration deadline) -> Result<Unit>
    {
        request_graceful_stop();
        std::atomic<bool> drained { false };
        std::thread watchdog([this, deadline, &drained] {
            const auto end = std::chrono::steady_clock::now() + deadline;
            while (std::chrono::steady_clock::now() < end) {
                if (drained.load(std::memory_order_acquire))
                    return; // 已 drain，无需硬停
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            stop(); // deadline 到 → 硬停残留在途
        });
        auto result = join(); // 等所有 worker drain（或被 watchdog 硬停）退出
        drained.store(true, std::memory_order_release);
        watchdog.join();
        return result;
    }

    // 等待所有线程结束。返回首个 core 的 run() 故障（若有）——让『退出码必须被看见』成为类型契约。
    [[nodiscard]] auto join() -> Result<Unit>
    {
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
        // 被移动后的 Executor 残壳 faults_mu_ 为 null（其析构会经 stop_and_join 走到这里）——守卫之。
        if (faults_mu_) {
            std::lock_guard lk(*faults_mu_);
            if (!faults_.empty()) {
                auto err = faults_.front();
                return Err(std::move(err));
            }
        }
        return Ok();
    }

    // 停止并 join（清理路径，丢弃故障——析构上下文无处传播）。
    auto stop_and_join() -> Unit
    {
        stop();
        (void)join();
        return unit;
    }

    [[nodiscard]] auto core_count() const noexcept -> usize { return loops_.size(); }

    // 访问指定 EventLoop（例如进行跨核心任务提交）。
    [[nodiscard]] auto loop_at(usize index) -> async::EventLoop& { return loops_[index]; }

private:
    explicit Executor(std::vector<async::EventLoop> loops) : loops_(std::move(loops)) { }

    [[nodiscard]] static auto ignore_sigpipe() -> Result<Unit>
    {
        struct sigaction action { };
        action.sa_handler = SIG_IGN;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;

        if (::sigaction(SIGPIPE, &action, nullptr) < 0) {
            return Err(SystemError::from_errno());
        }
        return Ok();
    }

    std::vector<async::EventLoop> loops_;
    std::vector<std::jthread> threads_;
    ThreadInitFn thread_init_;
    TeardownFn teardown_;
    std::deque<diag::CoreMetrics> metrics_; // 每核诊断计数器（deque：元素含 atomic 不可移动，但 deque 自身可移动）
    Box<std::mutex> faults_mu_ = std::make_unique<std::mutex>(); // Box：mutex 不可移动，装箱以保 Executor 可移动
    std::vector<SystemError> faults_;       // 各核 run() 故障（冷路径，仅故障时写）
    FaultPolicy policy_ = FaultPolicy::FailFast;
};

} // namespace rain::runtime
