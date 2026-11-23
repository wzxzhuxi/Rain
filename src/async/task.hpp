#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <coroutine>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace rain::async {
namespace detail {
template<typename T>
struct is_result : std::false_type { };

template<typename T, typename E>
struct is_result<Result<T, E>> : std::true_type { };
} // namespace detail

[[nodiscard]] inline auto make_cancelled_error(std::source_location loc = std::source_location::current())
    -> SystemError
{
    return SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                         .message = "task cancelled",
                         .location = loc };
}

template<typename T = Unit>
class JoinHandle;

// 惰性协程：initial_suspend 挂起，final_suspend 通过 continuation 做对称转移。
// 取消令牌沿 co_await 链向下传播，叶子 awaiter 挂 stop_callback 实现抢占式取消。
template<typename T = Unit>
class Task {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct promise_type {
        Result<T> result_ = Err(make_cancelled_error());
        std::coroutine_handle<> continuation_ { };
        // 本帧自有 stop-state：惰性分配，仅 request_stop()/stop_token() 真正使用才创建。
        std::stop_source stop_source_ { std::nostopstate };
        // 由取消根（timeout/when_any/JoinHandle）注入的令牌，await_transform 向下透传。
        std::stop_token cancel_token_ { };

        auto get_return_object() -> Task { return Task { handle_type::from_promise(*this) }; }

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
                if (auto cont = h.promise().continuation_)
                    return cont;
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

        auto ensure_stop_state() -> void
        {
            if (!stop_source_.stop_possible())
                stop_source_ = std::stop_source { };
        }
    };
    Task() = default;
    explicit Task(handle_type h) : handle_(h) { }

    Task(const Task&) = delete;
    auto operator=(const Task&) -> Task& = delete;

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, { })) { }

    auto operator=(Task&& other) noexcept -> Task&
    {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, { });
        }
        return *this;
    }

    ~Task() { reset(); }

    [[nodiscard]] auto await_ready() const noexcept -> bool { return !handle_ || handle_.done(); }

    auto await_suspend(std::coroutine_handle<> continuation) noexcept -> std::coroutine_handle<>
    {
        handle_.promise().continuation_ = continuation;
        return handle_;
    }

    // Task<Result<X>> 返回 Result<X>，错误自然传播；Task<Unit> 返回 unit。
    auto await_resume() -> T { return *std::move(handle_.promise().result_); }

    [[nodiscard]] auto get() -> Result<T>
    {
        if (!handle_) {
            return Err(SystemError { .code = std::make_error_code(std::errc::operation_not_permitted),
                                     .message = "task has no coroutine handle",
                                     .location = std::source_location::current() });
        }
        while (!handle_.done())
            handle_.resume();
        return std::move(handle_.promise().result_);
    }

    auto request_stop() -> Unit
    {
        if (handle_)
            handle_.promise().request_stop();
        return unit;
    }

    [[nodiscard]] auto stop_token() const -> std::stop_token
    {
        return handle_ ? handle_.promise().stop_token() : std::stop_token { };
    }

    auto set_cancellation(std::stop_token token) -> void
    {
        if (handle_)
            handle_.promise().cancel_token_ = std::move(token);
    }

    [[nodiscard]] auto handle() const noexcept -> handle_type { return handle_; }

private:
    auto reset() noexcept -> void
    {
        if (handle_)
            handle_.destroy();
        handle_ = { };
    }

    handle_type handle_ { };
};

// 已 spawn 任务的非拥有观察者：可等待，返回 Result<T>。
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
        if (handle_)
            handle_.promise().request_stop();
        return unit;
    }

    [[nodiscard]] auto await_ready() const noexcept -> bool { return done(); }

    auto await_suspend(std::coroutine_handle<> continuation) noexcept -> bool
    {
        if (!handle_ || handle_.done())
            return false;
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
