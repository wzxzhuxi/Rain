#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "config/config.hpp"

#include "app/shutdown.hpp"
#include "async/event_loop.hpp"
#include "async/task.hpp"
#include "http/accept_loop.hpp"
#include "http/handler.hpp"
#include "http/server_config.hpp"
#include "log/logger.hpp"
#include "net/tcp_listener.hpp"
#include "runtime/executor.hpp"

#include <atomic>
#include <concepts>
#include <functional>
#include <thread>
#include <type_traits>
#include <vector>

namespace rain::runtime {

// ── 钩子类型 ───────────────────────────────────────────────────────

// 在任意 accept 循环启动前，于 0 号核心执行的异步钩子。
using StartHook = std::move_only_function<async::Task<Result<Unit>>(async::EventLoop&, const config::Config&)>;

// 每个核心在自身 accept 循环启动前执行的异步 worker 钩子。
using WorkerStartHook
    = std::move_only_function<async::Task<Result<Unit>>(async::EventLoop&, usize /*core_id*/, const config::Config&)>;

// 每个核心线程在 EventLoop 退出后执行的同步钩子。
using WorkerStopHook = std::function<Result<Unit>(usize /*core_id*/, const config::Config&)>;

// 所有核心退出后，在主线程执行的同步钩子。
using StopHook = std::function<Result<Unit>(const config::Config&)>;

// ── 生命周期管理器（LifecycleManager）────────────────────────────────
// 持有全部生命周期钩子，并为每个核心提供统一入口。
//
// 执行顺序：
//   [core 0]  run_global_init     → on_start
//   [per core] run_core           → on_worker_start → accept_loop
//   [per core] (EventLoop 退出)
//   [per core] run_core_teardown  → on_worker_stop
//   [main]    run_global_teardown → on_stop

class LifecycleManager {
public:
    // ── 注册 ───────────────────────────────────────────────────────

    auto set_on_start(StartHook hook) -> Unit
    {
        on_start_ = std::move(hook);
        return unit;
    }

    template<typename Hook>
        requires std::invocable<Hook&, const config::Config&>
    auto add_on_stop(Hook hook) -> Unit
    {
        on_stops_.push_back([h = std::move(hook)](const config::Config& config) mutable -> Result<Unit> {
            using Ret = std::invoke_result_t<Hook&, const config::Config&>;
            if constexpr (std::convertible_to<Ret, Result<Unit>>) {
                return h(config);
            } else {
                h(config);
                return Ok();
            }
        });
        return unit;
    }

    auto add_on_worker_start(WorkerStartHook hook) -> Unit
    {
        on_worker_starts_.push_back(std::move(hook));
        return unit;
    }

    template<typename Hook>
        requires std::invocable<Hook&, usize, const config::Config&>
    auto add_on_worker_stop(Hook hook) -> Unit
    {
        on_worker_stops_.push_back(
            [h = std::move(hook)](usize core_id, const config::Config& config) mutable -> Result<Unit> {
                using Ret = std::invoke_result_t<Hook&, usize, const config::Config&>;
                if constexpr (std::convertible_to<Ret, Result<Unit>>) {
                    return h(core_id, config);
                } else {
                    h(core_id, config);
                    return Ok();
                }
            });
        return unit;
    }

    [[nodiscard]] auto has_on_start() const noexcept -> bool { return static_cast<bool>(on_start_); }

    // ── 全局初始化（0 号核心，异步）────────────────────────────────
    // 执行 on_start，并向其他核心发出完成信号。

    [[nodiscard]] auto run_global_init(async::EventLoop& loop, const config::Config& config)
        -> async::Task<Result<Unit>>
    {
        if (on_start_) {
            auto result = co_await on_start_(loop, config);
            if (!result) {
                init_error_->store(true, std::memory_order_release);
                init_done_->store(true, std::memory_order_release);
                co_return result;
            }
        }
        init_done_->store(true, std::memory_order_release);
        co_return Ok();
    }

    // ── 每核生命周期（异步）────────────────────────────────────────
    // 等待全局初始化，执行 worker_start 钩子，然后启动 accept。

    [[nodiscard]] auto run_core(async::EventLoop& loop,
                                usize core_id,
                                const config::Config& config,
                                log::Logger& logger,
                                Shared<http::HandlerFn> handler,
                                http::ServerConfig srv_config) -> async::Task<Unit>
    {
        // 非 0 号核心：等待全局初始化
        if (core_id != 0) {
            while (!init_done_->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (init_error_->load(std::memory_order_acquire)) {
                co_return unit;
            }
        }

        // 执行 worker start 钩子
        for (auto& hook : on_worker_starts_) {
            auto result = co_await hook(loop, core_id, config);
            if (!result) {
                if (auto log_result = logger.log(
                        log::Level::Error, "Core {} worker_start hook failed: {}", core_id, result.error().message);
                    !log_result) {
                    // worker_start 失败已记录到 result；日志失败不再覆盖主错误语义。
                }
            }
        }

        // 启动 accept 循环
        auto listener = net::TcpListener::bind(loop.reactor(), srv_config.bind_address, srv_config.listen_opts);
        if (listener) {
            auto lptr = std::make_shared<net::TcpListener>(std::move(*listener));
            loop.spawn(http::accept_loop(lptr, handler, srv_config));
        } else {
            if (auto log_result = logger.log(
                    log::Level::Error, "Core {} failed to bind listener: {}", core_id, listener.error().message);
                !log_result) {
                // bind 失败由 listener.error() 表示；日志失败不再叠加传播。
            }
        }

        co_return unit;
    }

    // ── 每核清理（同步，在 loop.run() 返回后调用）────────────────

    [[nodiscard]] auto run_core_teardown(usize core_id, const config::Config& config) -> Result<Unit>
    {
        for (auto& hook : on_worker_stops_) {
            auto result = hook(core_id, config);
            if (!result) {
                return Err(std::move(result).error());
            }
        }
        return Ok();
    }

    // ── 全局清理（主线程，同步）────────────────────────────────────

    [[nodiscard]] auto run_global_teardown(const config::Config& config) -> Result<Unit>
    {
        for (auto& hook : on_stops_) {
            auto result = hook(config);
            if (!result) {
                return Err(std::move(result).error());
            }
        }
        return Ok();
    }

private:
    StartHook on_start_;
    std::vector<StopHook> on_stops_;
    std::vector<WorkerStartHook> on_worker_starts_;
    std::vector<WorkerStopHook> on_worker_stops_;

    Shared<std::atomic<bool>> init_done_ = std::make_shared<std::atomic<bool>>(false);
    Shared<std::atomic<bool>> init_error_ = std::make_shared<std::atomic<bool>>(false);
};

} // namespace rain::runtime
