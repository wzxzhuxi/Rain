#pragma once

#include "async/task.hpp"
#include "async/event_loop.hpp"
#include "async/timer.hpp"
#include "core/types.hpp"
#include "core/result.hpp"

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace rain::async {

// ── 错误辅助函数 ─────────────────────────────────────────────────────

[[nodiscard]] inline auto make_timed_out_error(
    std::source_location loc = std::source_location::current()) -> SystemError
{
    return SystemError{
        .code    = std::make_error_code(std::errc::timed_out),
        .message = "operation timed out",
        .location = loc
    };
}

// ── 超时校验（with_deadline_check）───────────────────────────────────
// 等待任务完成，并检查实际耗时是否在预算内。
// 这是事后检查，不是抢占式取消。
// 若要实现真正的抢占式超时，内部操作需配合
// stop_token / deadline 以及事件循环定时器。

template<typename T>
[[nodiscard]] auto with_deadline_check(Task<Result<T>> task, Duration timeout)
    -> Task<Result<T>>
{
    const auto deadline = Clock::now() + timeout;
    auto result = co_await std::move(task);
    if (Clock::now() > deadline) {
        co_return Err(make_timed_out_error());
    }
    co_return std::move(result);
}

// ── 管道运算符标签类型 ───────────────────────────────────────────────

namespace detail {

template<typename F>
struct AndThenOp { F func; };

template<typename F>
struct MapOp { F func; };

template<typename F>
struct MapErrOp { F func; };

template<typename F>
struct OrElseOp { F func; };

template<typename F>
struct InspectOp { F func; };

template<typename F>
struct InspectErrOp { F func; };

// 从 Task<T> 中提取 T
template<typename>
struct task_value;

template<typename T>
struct task_value<Task<T>> { using type = T; };

template<typename T>
using task_value_t = typename task_value<T>::type;

} // namespace detail

// ── 管道工厂函数 ─────────────────────────────────────────────────────
// 这些函数创建轻量标签对象，供 operator| 分发。
// 用法：task | and_then(f) | map(g) | or_else(recover)

template<typename F>
[[nodiscard]] constexpr auto and_then(F&& f) {
    return detail::AndThenOp<std::decay_t<F>>{std::forward<F>(f)};
}

template<typename F>
[[nodiscard]] constexpr auto map(F&& f) {
    return detail::MapOp<std::decay_t<F>>{std::forward<F>(f)};
}

template<typename F>
[[nodiscard]] constexpr auto map_err(F&& f) {
    return detail::MapErrOp<std::decay_t<F>>{std::forward<F>(f)};
}

template<typename F>
[[nodiscard]] constexpr auto or_else(F&& f) {
    return detail::OrElseOp<std::decay_t<F>>{std::forward<F>(f)};
}

template<typename F>
[[nodiscard]] constexpr auto inspect(F&& f) {
    return detail::InspectOp<std::decay_t<F>>{std::forward<F>(f)};
}

template<typename F>
[[nodiscard]] constexpr auto inspect_err(F&& f) {
    return detail::InspectErrOp<std::decay_t<F>>{std::forward<F>(f)};
}

// ── 管道运算符（operator|）──────────────────────────────────────────

// 链式继续 and_then：f(T) -> Task<Result<U>>
// 成功时应用 f，并 co_await 其返回任务。
// 失败时原样传播错误。
template<typename T, typename F>
    requires std::invocable<F, T>
[[nodiscard]] auto operator|(Task<Result<T>> task, detail::AndThenOp<F> op)
    -> std::invoke_result_t<F, T>
{
    auto result = co_await std::move(task);
    if (!result) {
        co_return Err(std::move(result).error());
    }
    co_return co_await std::move(op.func)(std::move(*result));
}

// 值映射 map：f(T) -> U（纯函数、同步）
// 成功时将 f(value) 包装为 Ok。
// 失败时原样传播错误。
template<typename T, typename F>
    requires std::invocable<F, T>
[[nodiscard]] auto operator|(Task<Result<T>> task, detail::MapOp<F> op)
    -> Task<Result<std::invoke_result_t<F, T>>>
{
    auto result = co_await std::move(task);
    if (!result) {
        co_return Err(std::move(result).error());
    }
    co_return Ok(std::move(op.func)(std::move(*result)));
}

// 错误映射 map_err：f(SystemError) -> SystemError
// 失败时转换错误。
// 成功时透传原值。
template<typename T, typename F>
    requires std::invocable<F, SystemError>
[[nodiscard]] auto operator|(Task<Result<T>> task, detail::MapErrOp<F> op)
    -> Task<Result<T>>
{
    auto result = co_await std::move(task);
    if (!result) {
        co_return Err(std::move(op.func)(std::move(result).error()));
    }
    co_return Ok(std::move(*result));
}

// 失败兜底 or_else：f(SystemError) -> Task<Result<T>>
// 失败时通过 co_await f(error) 尝试恢复。
// 成功时透传原值。
template<typename T, typename F>
    requires std::invocable<F, SystemError>
[[nodiscard]] auto operator|(Task<Result<T>> task, detail::OrElseOp<F> op)
    -> Task<Result<T>>
{
    auto result = co_await std::move(task);
    if (!result) {
        co_return co_await std::move(op.func)(std::move(result).error());
    }
    co_return Ok(std::move(*result));
}

// 成功旁路 inspect：f(const T&) -> void
// 成功时调用 f 执行副作用（日志、指标等），且不消费该值。
// 结果保持不变并继续传播。
template<typename T, typename F>
    requires std::invocable<F, const T&>
[[nodiscard]] auto operator|(Task<Result<T>> task, detail::InspectOp<F> op)
    -> Task<Result<T>>
{
    auto result = co_await std::move(task);
    if (result) {
        op.func(*result);
    }
    co_return std::move(result);
}

// 失败旁路 inspect_err：f(const SystemError&) -> void
// 失败时调用 f 执行副作用，且不消费该错误。
template<typename T, typename F>
    requires std::invocable<F, const SystemError&>
[[nodiscard]] auto operator|(Task<Result<T>> task, detail::InspectErrOp<F> op)
    -> Task<Result<T>>
{
    auto result = co_await std::move(task);
    if (!result) {
        op.func(result.error());
    }
    co_return std::move(result);
}

// ── 全部等待（when_all）──────────────────────────────────────────────
// 并发执行所有任务：全部 spawn 到 EventLoop，各 racer 把结果写入按下标
// 定位的共享存储，全部完成后唤醒等待者。

namespace detail {

template<typename T>
struct WhenAllState {
    usize remaining = 0;
    std::vector<Result<T>> results;
    std::coroutine_handle<> waiter { };
};

template<typename T>
[[nodiscard]] auto when_all_racer(usize idx, Task<Result<T>> task, Shared<WhenAllState<T>> state)
    -> Task<Unit>
{
    state->results[idx] = co_await std::move(task);
    if (--state->remaining == 0 && state->waiter && g_event_loop) {
        g_event_loop->notify(state->waiter);
    }
    co_return unit;
}

template<typename T>
class WhenAllAwaiter {
public:
    explicit WhenAllAwaiter(Shared<WhenAllState<T>> state) : state_(std::move(state)) { }

    [[nodiscard]] auto await_ready() const noexcept -> bool { return state_->remaining == 0; }

    auto await_suspend(std::coroutine_handle<> h) noexcept -> bool
    {
        state_->waiter = h;
        return state_->remaining != 0;
    }

    [[nodiscard]] auto await_resume() -> std::vector<Result<T>> { return std::move(state_->results); }

private:
    Shared<WhenAllState<T>> state_;
};

} // namespace detail

template<typename T>
[[nodiscard]] auto when_all(std::vector<Task<Result<T>>> tasks)
    -> Task<std::vector<Result<T>>>
{
    auto state = std::make_shared<detail::WhenAllState<T>>();
    state->remaining = tasks.size();
    state->results.reserve(tasks.size());
    for (usize i = 0; i < tasks.size(); ++i) {
        state->results.emplace_back(Err(make_cancelled_error()));
    }

    // 即发即弃：racer 归事件循环所有，不持有 JoinHandle，避免读取已被回收的协程帧（UAF）。
    usize idx = 0;
    for (auto& task : tasks) {
        g_event_loop->spawn(detail::when_all_racer(idx++, std::move(task), state));
    }

    co_return co_await detail::WhenAllAwaiter<T>(state);
}

// ── 任一完成（when_any）─────────────────────────────────────────────
// 并发执行所有任务，第一个成功的结果立即返回，取消其余。
// 使用 EventLoop::notify 实现零轮询唤醒。

namespace detail {

template<typename T>
struct WhenAnyState {
    bool resolved = false;
    usize remaining = 0;
    Result<T> result = Err(make_cancelled_error());
    std::coroutine_handle<> waiter { };
};

template<typename T>
[[nodiscard]] auto when_any_racer(Task<Result<T>> task, Shared<WhenAnyState<T>> state)
    -> Task<Unit>
{
    auto r = co_await std::move(task);
    // 首个成功者胜出；若全部失败，则最后一个完成者带着错误唤醒等待者，避免永久挂起。
    const bool last = (--state->remaining == 0);
    if (!state->resolved && (r || last)) {
        state->resolved = true;
        state->result = std::move(r);
        if (state->waiter && g_event_loop) {
            g_event_loop->notify(state->waiter);
        }
    }
    co_return unit;
}

template<typename T>
class WhenAnyAwaiter {
public:
    explicit WhenAnyAwaiter(Shared<WhenAnyState<T>> state) : state_(std::move(state)) { }

    [[nodiscard]] auto await_ready() const noexcept -> bool { return state_->resolved; }

    auto await_suspend(std::coroutine_handle<> h) noexcept -> bool
    {
        state_->waiter = h;
        return !state_->resolved;
    }

    [[nodiscard]] auto await_resume() -> Result<T>
    {
        return std::move(state_->result);
    }

private:
    Shared<WhenAnyState<T>> state_;
};

} // namespace detail

template<typename T>
[[nodiscard]] auto when_any(std::vector<Task<Result<T>>> tasks)
    -> Task<Result<T>>
{
    if (tasks.empty()) {
        co_return Err(make_cancelled_error());
    }

    auto state = std::make_shared<detail::WhenAnyState<T>>();
    state->remaining = tasks.size();

    // 即发即弃：racer 归事件循环所有，落败者在自然完成时回收。
    // 不持有 JoinHandle——对已完成（帧已销毁）的 racer 调用 request_stop 是 UAF，
    // 且协作式取消下无 awaiter 读取 stop_token，该调用本就无效。
    for (auto& task : tasks) {
        g_event_loop->spawn(detail::when_any_racer(std::move(task), state));
    }

    co_await detail::WhenAnyAwaiter<T>(state);

    co_return std::move(state->result);
}

// ── 重试机制（retry）─────────────────────────────────────────────────
// 将任务工厂最多重试 max_attempts 次。
// 每次尝试都会调用工厂生成一个新的 Task。
// 若有定时轮，则在尝试之间等待 delay_between。

template<typename T, typename Factory>
    requires std::invocable<Factory>
[[nodiscard]] auto retry(Factory factory, u32 max_attempts, Duration delay_between = Duration::zero())
    -> Task<Result<T>>
{
    for (u32 attempt = 0; attempt < max_attempts; ++attempt) {
        auto result = co_await factory();
        if (result) {
            co_return std::move(result);
        }

        const bool last_attempt = (attempt + 1 >= max_attempts);
        if (last_attempt) {
            co_return std::move(result);
        }

        if (delay_between > Duration::zero() && g_timer) {
            co_await sleep_for(*g_timer, delay_between);
        }
    }

    // 理论不可达，仅用于满足编译器控制流检查。
    co_return Err(make_cancelled_error());
}

} // namespace rain::async
