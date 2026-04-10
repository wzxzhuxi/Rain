#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <chrono>
#include <concepts>
#include <coroutine>
#include <optional>

namespace rain::async {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;
using TimerId = u64;

enum class WaitState : u8 {
    Pending = 0,
    Ready,
    TimedOut,
};

// ── IoEvent——抽象 I/O 事件标志 ───────────────────────────────────────
// 与后端无关的位定义。映射到平台特定取值
// （epoll、io_uring、kqueue）由 reactor 实现完成。

enum class IoEvent : u32 {
    None = 0,
    Read = 1u << 0,
    Write = 1u << 1,
    Error = 1u << 2,
    HangUp = 1u << 3,
    EdgeTrig = 1u << 4,
    OneShot = 1u << 5,
};

[[nodiscard]] constexpr auto operator|(IoEvent lhs, IoEvent rhs) -> IoEvent
{
    return static_cast<IoEvent>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

[[nodiscard]] constexpr auto operator&(IoEvent lhs, IoEvent rhs) -> IoEvent
{
    return static_cast<IoEvent>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}

[[nodiscard]] constexpr auto operator~(IoEvent val) -> IoEvent
{
    return static_cast<IoEvent>(~static_cast<u32>(val));
}

[[nodiscard]] constexpr auto has_flag(IoEvent set, IoEvent flag) -> bool
{
    return (set & flag) == flag;
}

// ── 概念定义 ─────────────────────────────────────────────────────────

template<typename T>
concept Awaitable = requires(T a) {
    { a.await_ready() } -> std::convertible_to<bool>;
    a.await_suspend(std::coroutine_handle<> { });
    a.await_resume();
};

template<typename R>
concept IoReactorLike = requires(R r, i32 fd, IoEvent ev, Duration timeout) {
    { r.add(fd, ev) } -> std::same_as<Result<Unit>>;
    { r.modify(fd, ev) } -> std::same_as<Result<Unit>>;
    { r.remove(fd) } -> std::same_as<Result<Unit>>;
    { r.poll(timeout) } -> std::same_as<Result<u32>>;
};

template<typename T>
concept TimerLike = requires(T t, Duration d, std::coroutine_handle<> h, TimerId id) {
    { t.schedule(d, h) } -> std::same_as<TimerId>;
    { t.cancel(id) } -> std::same_as<bool>;
    { t.tick() } -> std::same_as<u32>;
    { t.next_deadline() } -> std::same_as<std::optional<TimePoint>>;
};

} // namespace rain::async
