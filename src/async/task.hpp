#pragma once

#include "core/diagnostics.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <cassert>
#include <coroutine>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace rain::async {

// ---------------------------------------------------------------------------
// 细节：用于检测 Result 的类型萃取
// ---------------------------------------------------------------------------

namespace detail {

template<typename T>
struct is_result : std::false_type { };

template<typename T, typename E>
struct is_result<Result<T, E>> : std::true_type { };

template<typename T>
struct task_resume_type {
    using type = T;
};

} // namespace detail

// ---------------------------------------------------------------------------
// 取消辅助函数
// ---------------------------------------------------------------------------

[[nodiscard]] inline auto make_cancelled_error(std::source_location loc = std::source_location::current())
    -> SystemError
{
    return SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                         .message = "task cancelled",
                         .location = loc };
}

// ---------------------------------------------------------------------------
// 前向声明
// ---------------------------------------------------------------------------

template<typename T = Unit>
class JoinHandle;

// ---------------------------------------------------------------------------
// Task<T>——支持对称转移与取消的惰性协程
// ---------------------------------------------------------------------------

template<typename T = Unit>
class Task {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    // -- promise_type ------------------------------------------------------
    // 仅提供 return_value，不提供 return_void。对于 Task<Unit>，
    // 用户写 co_return unit;。这可彻底规避 GCC 中
    // return_void / return_value 共存问题。

    struct promise_type {
        Result<T> result_ = Err(make_cancelled_error());
        std::coroutine_handle<> continuation_ { };
        // 本帧自有的 stop-state（区别于下面传播进来的 cancel_token_）：惰性 nostopstate 默认
        // 构造不分配控制块，仅 Task::request_stop()/stop_token() 显式使用时才创建。抢占式取消
        // 走的是 cancel_token_（由 timeout/when_any 注入、await_transform 向下传播、叶子挂
        // stop_callback），不依赖此 stop_source_，故此字段仍极少分配，保住惰性快路径。
        // 前提：取消为 owner 核驱动——stop_callback 在 request_stop 的调用线程同步执行，会触碰
        // reactor/timer（非线程安全），跨核 request_stop 需调用方避免。
        std::stop_source stop_source_ { std::nostopstate };

        // 取消令牌沿 co_await 链向下传播：本帧收到的取消令牌（默认空）。由取消根
        // （timeout/when_any/JoinHandle）经 Task::set_cancellation 注入；await_transform 把它
        // 透传给被 co_await 的子帧 / IO awaiter，叶子据此挂 stop_callback 实现抢占式取消。
        std::stop_token cancel_token_ {};

        auto get_return_object() -> Task { return Task { handle_type::from_promise(*this) }; }

        // 拦截本帧所有 co_await：若被等待者支持取消（有 set_cancellation 成员）且本帧持有有效
        // 取消令牌，则把令牌注入它，使取消沿链向下传播；否则恒等透传——关闭路径（token 空）为
        // 纯 no-op，不改变任何现有 co_await 语义、零分配。
        template<typename A>
        auto await_transform(A&& a) -> A&&
        {
            if constexpr (requires { a.set_cancellation(cancel_token_); }) {
                if (cancel_token_.stop_possible())
                    a.set_cancellation(cancel_token_);
            }
            return std::forward<A>(a);
        }

        auto initial_suspend() noexcept -> std::suspend_always { return { }; }

        struct FinalAwaiter {
            auto await_ready() noexcept -> bool { return false; }

            auto await_suspend(handle_type h) noexcept -> std::coroutine_handle<>
            {
                if (auto cont = h.promise().continuation_) {
                    return cont;
                }
                return std::noop_coroutine();
            }

            void await_resume() noexcept { }
        };

        auto final_suspend() noexcept -> FinalAwaiter { return { }; }

        template<typename U>
            requires std::convertible_to<U, T>
        void return_value(U&& value)
        {
            result_ = Result<T>(std::in_place, T(std::forward<U>(value)));
        }

        void unhandled_exception()
        {
            diag::count(&diag::CoreMetrics::coroutine_exceptions);
            result_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_not_permitted),
                                        .message = "unhandled exception",
                                        .location = std::source_location::current() });
        }

        [[nodiscard]] auto stop_token() -> std::stop_token
        {
            ensure_stop_state();
            return stop_source_.get_token();
        }

        auto request_stop() -> Unit
        {
            ensure_stop_state();
            stop_source_.request_stop();
            return unit;
        }

        // 惰性创建 stop-state：仅在首次真正需要取消能力时分配。nostopstate 的
        // stop_possible() 为 false，据此判断尚未分配；已分配（含已请求停止）则为 true。
        auto ensure_stop_state() -> void
        {
            if (!stop_source_.stop_possible())
                stop_source_ = std::stop_source { };
        }
    };

    // -- 构造与五法则 ------------------------------------------------------

    Task() = default;

    explicit Task(handle_type h) : handle_(h) { }

    Task(const Task&) = delete;
    auto operator=(const Task&) -> Task& = delete;

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, { })) { }

    auto operator=(Task&& other) noexcept -> Task&
    {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, { });
        }
        return *this;
    }

    ~Task()
    {
        if (handle_) {
            handle_.destroy();
        }
    }

    // -- Awaitable 接口 ----------------------------------------------------

    [[nodiscard]] auto await_ready() const noexcept -> bool { return !handle_ || handle_.done(); }

    auto await_suspend(std::coroutine_handle<> continuation) noexcept -> std::coroutine_handle<>
    {
        handle_.promise().continuation_ = continuation;
        return handle_;
    }

    // 通过未检查解包（operator*）返回 T。
    // 对 Task<Result<X>>：返回 Result<X>，错误自然传播。
    // 对 Task<int>：若任务被取消，这里是 UB。可失败操作应使用
    // Task<Result<int>> 以获得安全错误传播。
    auto await_resume() -> T { return *std::move(handle_.promise().result_); }

    // -- 同步访问（仅测试）-------------------------------------------------

    [[nodiscard]] auto get() -> Result<T>
    {
        if (!handle_) {
            return Err(SystemError { .code = std::make_error_code(std::errc::operation_not_permitted),
                                     .message = "task has no coroutine handle",
                                     .location = std::source_location::current() });
        }
        while (!handle_.done()) {
            handle_.resume();
        }
        return std::move(handle_.promise().result_);
    }

    // -- 取消 --------------------------------------------------------------

    auto request_stop() -> Unit
    {
        if (handle_) {
            handle_.promise().request_stop();
        }
        return unit;
    }

    [[nodiscard]] auto stop_token() const -> std::stop_token
    {
        if (handle_) {
            return handle_.promise().stop_token();
        }
        return { };
    }

    // 注入外部取消令牌：取消根（timeout/when_any/JoinHandle）在本 Task 开跑前调用，
    // 令牌经本帧 promise 的 await_transform 沿 co_await 链向下传播到叶子 IO awaiter。
    auto set_cancellation(std::stop_token token) -> void
    {
        if (handle_) {
            handle_.promise().cancel_token_ = std::move(token);
        }
    }

    // -- 句柄访问（供 JoinHandle 使用）------------------------------------

    [[nodiscard]] auto handle() const noexcept -> handle_type { return handle_; }

private:
    handle_type handle_ { };
};

// ---------------------------------------------------------------------------
// JoinHandle<T>——已 spawn 任务的非拥有观察者
// ---------------------------------------------------------------------------

template<typename T>
class JoinHandle {
public:
    using promise_type = typename Task<T>::promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    explicit JoinHandle(handle_type h) : handle_(h) { }

    JoinHandle(const JoinHandle&) = delete;
    auto operator=(const JoinHandle&) -> JoinHandle& = delete;

    JoinHandle(JoinHandle&& other) noexcept : handle_(std::exchange(other.handle_, { })) { }

    auto operator=(JoinHandle&& other) noexcept -> JoinHandle&
    {
        handle_ = std::exchange(other.handle_, { });
        return *this;
    }

    [[nodiscard]] auto done() const noexcept -> bool { return !handle_ || handle_.done(); }

    auto request_stop() -> Unit
    {
        if (handle_) {
            handle_.promise().request_stop();
        }
        return unit;
    }

    // -- Awaitable 接口（返回 Result<T>）----------------------------------

    [[nodiscard]] auto await_ready() const noexcept -> bool { return done(); }

    auto await_suspend(std::coroutine_handle<> continuation) noexcept -> bool
    {
        if (!handle_ || handle_.done()) {
            return false;
        }
        handle_.promise().continuation_ = continuation;
        return true;
    }

    auto await_resume() -> Result<T>
    {
        if (!handle_) {
            return Err(SystemError { .code = std::make_error_code(std::errc::operation_not_permitted),
                                     .message = "join handle has no task",
                                     .location = std::source_location::current() });
        }
        return std::move(handle_.promise().result_);
    }

private:
    handle_type handle_ { };
};

} // namespace rain::async
