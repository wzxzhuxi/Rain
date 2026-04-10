#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/event_loop.hpp"

#include <functional>
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

        for (usize i = 0; i < loops_.size(); ++i) {
            auto setup_result = setup(loops_[i], i);
            if (!setup_result) {
                stop_and_join();
                return Err(std::move(setup_result).error());
            }

            threads_.emplace_back([this, i] {
                if (thread_init_)
                    thread_init_(i);
                (void)loops_[i].run();
                if (teardown_)
                    teardown_(i);
            });
        }

        return Ok();
    }

    // 请求所有 EventLoop 停止（非阻塞）。
    auto stop() -> Unit
    {
        for (auto& loop : loops_) {
            loop.request_stop();
        }
        return unit;
    }

    // 等待所有线程结束。
    auto join() -> Unit
    {
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
        return unit;
    }

    // 停止并 join。
    auto stop_and_join() -> Unit
    {
        stop();
        join();
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
};

} // namespace rain::runtime
