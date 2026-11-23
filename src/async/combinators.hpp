#pragma once

#include "async/event_loop.hpp"
#include "async/task.hpp"
#include "async/timer.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <memory>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

namespace rain::async {
[[nodiscard]] inline auto make_timed_out_error(std::source_location loc = std::source_location::current())
    -> SystemError
{
    return SystemError { .code = std::make_error_code(std::errc::timed_out),
                         .message = "operation timed out",
                         .location = loc };
}

// 事后截止检查，不是抢占式取消；抢占式请用 timeout()。
template<typename T>
[[nodiscard]] auto with_deadline_check(Task<Result<T>> task, Duration timeout) -> Task<Result<T>>
{
    const auto deadline = Clock::now() + timeout;
    auto result = co_await std::move(task);
    if (Clock::now() > deadline)
        co_return Err(make_timed_out_error());
    co_return std::move(result);
}

// ── 管道运算符：纯标签对象 + operator|，组合优于继承 ──

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
} // namespace detail

template<typename F>
[[nodiscard]] constexpr auto and_then(F&& f)
{
    return detail::AndThenOp<std::decay_t<F>> { std::forward<F>(f) };
}

template<typename F>
[[nodiscard]] constexpr auto map(F&& f)
{
    return detail::MapOp<std::decay_t<F>> { std::forward<F>(f) };
}

template<typename F>
[[nodiscard]] constexpr auto map_err(F&& f)
{
    return detail::MapErrOp<std::decay_t<F>> { std::forward<F>(f) };
}

template<typename F>
[[nodiscard]] constexpr auto or_else(F&& f)
{
    return detail::OrElseOp<std::decay_t<F>> { std::forward<F>(f) };
}

template<typename F>
[[nodiscard]] constexpr auto inspect(F&& f)
{
    return detail::InspectOp<std::decay_t<F>> { std::forward<F>(f) };
}

template<typename F>
[[nodiscard]] constexpr auto inspect_err(F&& f)
{
    return detail::InspectErrOp<std::decay_t<F>> { std::forward<F>(f) };
}

template<typename T, typename F>
    requires std::invocable<F, T>
[[nodiscard]] auto operator|(Task<Result<T>> task, detail::AndThenOp<F> op) -> std::invoke_result_t<F, T>
{
    auto result = co_await std::move(task);
    if (!result)
        co_return Err(std::move(result).error());
    co_return co_await std::move(op.func)(std::move(*result));
}

template<typename T, typename F>
    requires std::invocable<F, T>
[[nodiscard]] auto operator|(Task<Result<T>> task, detail::MapOp<F> op)
    -> Task<Result<std::invoke_result_t<F, T>>>
{
    auto result = co_await std::move(task);
    if (!result)
        co_return Err(std::move(result).error());
    co_return Ok(std::move(op.func)(std::move(*result)));
}

template<typename T, typename F>
    requires std::invocable<F, SystemError>
[[nodiscard]] auto operator|(Task<Result<T>> task, detail::MapErrOp<F> op) -> Task<Result<T>>
{
    auto result = co_await std::move(task);
    if (!result)
        co_return Err(std::move(op.func)(std::move(result).error()));
    co_return Ok(std::move(*result));
}

template<typename T, typename F>
    requires std::invocable<F, SystemError>
[[nodiscard]] auto operator|(Task<Result<T>> task, detail::OrElseOp<F> op) -> Task<Result<T>>
{
    auto result = co_await std::move(task);
    if (!result)
        co_return co_await std::move(op.func)(std::move(result).error());
    co_return Ok(std::move(*result));
}

template<typename T, typename F>
    requires std::invocable<F, const T&>
[[nodiscard]] auto operator|(Task<Result<T>> task, detail::InspectOp<F> op) -> Task<Result<T>>
{
    auto result = co_await std::move(task);
    if (result)
        op.func(*result);
    co_return std::move(result);
}

template<typename T, typename F>
    requires std::invocable<F, const SystemError&>
[[nodiscard]] auto operator|(Task<Result<T>> task, detail::InspectErrOp<F> op) -> Task<Result<T>>
{
    auto result = co_await std::move(task);
    if (!result)
        op.func(result.error());
    co_return std::move(result);
}

// ── when_all：全部完成后按下标返回结果 ──

namespace detail {
template<typename T>
struct WhenAllState {
    usize remaining = 0;
    std::vector<Result<T>> results;
    std::coroutine_handle<> waiter { };
};

template<typename T>
[[nodiscard]] auto when_all_racer(usize idx, Task<Result<T>> task, Shared<WhenAllState<T>> state) -> Task<Unit>
{
    state->results[idx] = co_await std::move(task);
    if (--state->remaining == 0 && state->waiter && g_event_loop)
        g_event_loop->notify(state->waiter);
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
[[nodiscard]] auto when_all(std::vector<Task<Result<T>>> tasks) -> Task<std::vector<Result<T>>>
{
    auto state = std::make_shared<detail::WhenAllState<T>>();
    state->remaining = tasks.size();
    state->results.reserve(tasks.size());
    for (usize i = 0; i < tasks.size(); ++i)
        state->results.emplace_back(Err(make_cancelled_error()));

    // racer 归事件循环所有，不持 JoinHandle——避免读取已回收帧。
    usize idx = 0;
    for (auto& task : tasks)
        g_event_loop->spawn(detail::when_all_racer(idx++, std::move(task), state));
    co_return co_await detail::WhenAllAwaiter<T>(state);
}

// ── when_any：首个成功者胜出，抢占取消其余 ──

namespace detail {
// when_any / timeout 共用的赛跑状态与等待器。
template<typename T>
struct RaceState {
    bool resolved = false;
    Result<T> result = Err(make_cancelled_error());
    std::coroutine_handle<> waiter { };
};

template<typename T, typename State>
class RaceAwaiter {
public:
    explicit RaceAwaiter(Shared<State> state) : state_(std::move(state)) { }

    [[nodiscard]] auto await_ready() const noexcept -> bool { return state_->resolved; }

    auto await_suspend(std::coroutine_handle<> h) noexcept -> bool
    {
        state_->waiter = h;
        return !state_->resolved;
    }

    [[nodiscard]] auto await_resume() -> Result<T> { return std::move(state_->result); }

private:
    Shared<State> state_;
};

template<typename T>
struct WhenAnyState : RaceState<T> {
    usize remaining = 0;
    std::vector<Shared<std::stop_source>> sources; // refcounted，与帧解耦 → 取消败者无 UAF
};

template<typename T>
[[nodiscard]] auto when_any_racer(Task<Result<T>> task, Shared<WhenAnyState<T>> state) -> Task<Unit>
{
    auto r = co_await std::move(task);
    const bool last = (--state->remaining == 0);
    if (!state->resolved && (r || last)) {
        state->resolved = true;
        state->result = std::move(r);
        for (auto& s : state->sources)
            s->request_stop();
        if (state->waiter && g_event_loop)
            g_event_loop->notify(state->waiter);
    }
    co_return unit;
}
} // namespace detail

template<typename T>
[[nodiscard]] auto when_any(std::vector<Task<Result<T>>> tasks) -> Task<Result<T>>
{
    if (tasks.empty())
        co_return Err(make_cancelled_error());
    auto state = std::make_shared<detail::WhenAnyState<T>>();
    state->remaining = tasks.size();
    state->sources.reserve(tasks.size());

    // 每个 racer 一个 shared stop_source：胜者 request_stop 全部，败者叶子回调据此唤醒收尾。
    for (auto& task : tasks) {
        auto src = std::make_shared<std::stop_source>();
        task.set_cancellation(src->get_token());
        state->sources.push_back(std::move(src));
        g_event_loop->spawn(detail::when_any_racer(std::move(task), state));
    }

    co_await detail::RaceAwaiter<T, detail::WhenAnyState<T>>(state);
    co_return std::move(state->result);
}

// ── timeout：任务与 sleep 哨兵赛跑，先完成者胜出并取消败者 ──

namespace detail {
template<typename T>
struct TimeoutState : RaceState<T> { };

template<typename T>
[[nodiscard]] inline auto timeout_task_racer(
    Task<Result<T>> task, Shared<TimeoutState<T>> state, std::shared_ptr<std::stop_source> timer_src)
    -> Task<Unit>
{
    auto r = co_await std::move(task);
    if (!state->resolved) {
        state->resolved = true;
        state->result = std::move(r);
        if (timer_src)
            timer_src->request_stop();
        if (state->waiter && g_event_loop)
            g_event_loop->notify(state->waiter);
    }
    co_return unit;
}

template<typename T>
[[nodiscard]] inline auto timeout_timer_racer(
    Duration dur, Shared<TimeoutState<T>> state, std::shared_ptr<std::stop_source> task_src) -> Task<Unit>
{
    auto slept = co_await sleep_for(*g_timer, dur);
    if (!slept)
        co_return unit; // task 先赢，哨兵退场
    if (!state->resolved) {
        state->resolved = true;
        state->result = Err(make_timed_out_error());
        if (task_src)
            task_src->request_stop();
        if (state->waiter && g_event_loop)
            g_event_loop->notify(state->waiter);
    }
    co_return unit;
}
} // namespace detail

template<typename T>
[[nodiscard]] auto timeout(Task<Result<T>> task, Duration dur) -> Task<Result<T>>
{
    auto state = std::make_shared<detail::TimeoutState<T>>();
    auto task_src = std::make_shared<std::stop_source>();
    auto timer_src = std::make_shared<std::stop_source>();
    task.set_cancellation(task_src->get_token());
    auto timer_racer = detail::timeout_timer_racer<T>(dur, state, task_src);
    timer_racer.set_cancellation(timer_src->get_token());
    g_event_loop->spawn(detail::timeout_task_racer<T>(std::move(task), state, timer_src));
    g_event_loop->spawn(std::move(timer_racer));
    co_await detail::RaceAwaiter<T, detail::TimeoutState<T>>(state);
    co_return std::move(state->result);
}

// ── retry：最多尝试 max_attempts 次，可带间隔 ──

template<typename T, typename Factory>
    requires std::invocable<Factory>
[[nodiscard]] auto retry(Factory factory, u32 max_attempts, Duration delay_between = Duration::zero())
    -> Task<Result<T>>
{
    for (u32 attempt = 0; attempt < max_attempts; ++attempt) {
        auto result = co_await factory();
        if (result)
            co_return std::move(result);
        if (attempt + 1 >= max_attempts)
            co_return std::move(result);
        if (delay_between > Duration::zero() && g_timer)
            co_await sleep_for(*g_timer, delay_between);
    }
    co_return Err(make_cancelled_error());
}
} // namespace rain::async
