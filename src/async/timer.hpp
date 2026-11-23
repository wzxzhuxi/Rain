#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/concepts.hpp"

#include <coroutine>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rain::async {
struct TimerEntry {
    TimerId id;
    TimePoint deadline;
    std::coroutine_handle<> handle;
    WaitState* state = nullptr;
    WaitState completion = WaitState::TimedOut;
};
static constexpr Duration kTickDuration = std::chrono::milliseconds(1);
static constexpr Duration kMaxDelay = kTickDuration * static_cast<i64>(u64 { 1 } << 26);

// 教学版定时器：以 deadline 为键的有序表，tick 推进 1ms 并触发所有到期条目。
// 相比分层时间轮牺牲 O(1) 插入/取消，换来直观实现；cancel 仍急切移除（无墓碑、无 UAF）。
class HierarchicalTimerWheel {
public:
    using EntryMap = std::multimap<TimePoint, TimerEntry>;

    HierarchicalTimerWheel() : current_time_(Clock::now()) { }

    [[nodiscard]] auto schedule(Duration delay, std::coroutine_handle<> handle) -> TimerId
    {
        return schedule_at(current_time_ + delay, handle);
    }

    [[nodiscard]] auto schedule(Duration delay, std::coroutine_handle<> handle, WaitState* state,
                                WaitState completion = WaitState::TimedOut) -> TimerId
    {
        return schedule_at(current_time_ + delay, handle, state, completion);
    }

    [[nodiscard]] auto schedule_at(TimePoint deadline, std::coroutine_handle<> handle) -> TimerId
    {
        return schedule_at(deadline, handle, nullptr, WaitState::TimedOut);
    }

    [[nodiscard]] auto schedule_at(TimePoint deadline, std::coroutine_handle<> handle, WaitState* state,
                                   WaitState completion = WaitState::TimedOut) -> TimerId
    {
        const TimerId id = next_id_++;
        auto it = timers_.emplace(deadline, TimerEntry { .id = id,
                                                         .deadline = deadline,
                                                         .handle = handle,
                                                         .state = state,
                                                         .completion = completion });
        index_.emplace(id, it);
        return id;
    }

    // 立即擦除两张表：取消后即使调用方销毁 WaitState，也不会再被解引用。
    [[nodiscard]] auto cancel(TimerId id) -> bool
    {
        const auto it = index_.find(id);
        if (it == index_.end())
            return false;
        timers_.erase(it->second);
        index_.erase(it);
        return true;
    }

    [[nodiscard]] auto tick() -> u32
    {
        current_time_ += kTickDuration;
        u32 fired = 0;
        while (!timers_.empty() && timers_.begin()->first <= current_time_) {
            auto node = timers_.extract(timers_.begin());
            TimerEntry entry = std::move(node.mapped());
            index_.erase(entry.id);
            if (entry.state) {
                if (*entry.state != WaitState::Pending)
                    continue;
                *entry.state = entry.completion;
            }
            if (entry.handle && !entry.handle.done()) {
                ready_.push_back(entry.handle);
                ++fired;
            }
        }
        return fired;
    }

    [[nodiscard]] auto advance_to(TimePoint target) -> u32
    {
        u32 total = 0;
        while (current_time_ < target)
            total += tick();
        return total;
    }

    [[nodiscard]] auto next_deadline() const -> std::optional<TimePoint>
    {
        if (timers_.empty())
            return std::nullopt;
        return timers_.begin()->first;
    }

    [[nodiscard]] auto drain_ready() -> std::vector<std::coroutine_handle<>> { return std::exchange(ready_, { }); }

    auto drain_ready_into(std::vector<std::coroutine_handle<>>& out) -> void
    {
        out.insert(out.end(), ready_.begin(), ready_.end());
        ready_.clear();
    }

    [[nodiscard]] auto current_time() const noexcept -> TimePoint { return current_time_; }

    // timer 层不能反向依赖 event_loop；取消唤醒借 ready_ 由事件循环排空。
    auto wake(std::coroutine_handle<> h) -> void { ready_.push_back(h); }

private:
    EntryMap timers_;
    std::unordered_map<TimerId, EntryMap::iterator> index_;
    TimerId next_id_ = 1;
    TimePoint current_time_;
    std::vector<std::coroutine_handle<>> ready_;
};

// --- 睡眠等待器（SleepAwaiter）---

class SleepAwaiter {
public:
    SleepAwaiter(HierarchicalTimerWheel& wheel, Duration delay) : wheel_(wheel), delay_(delay) { }

    // 注入取消令牌：sleep 被取消时立即唤醒，不再悬挂 timer。
    auto set_cancellation(std::stop_token token) -> void { token_ = std::move(token); }

    [[nodiscard]] auto await_ready() const noexcept -> bool { return delay_ <= Duration::zero(); }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle_ = h;
        // fire 做 Pending→Ready，与取消回调的 Pending→Cancelled 在 owner 核上串行竞争。
        id_ = wheel_.schedule(delay_, h, &state_, WaitState::Ready);
        if (token_.stop_possible()) {
            cancel_cb_.emplace(token_, [this] {
                if (state_ != WaitState::Pending)
                    return;
                state_ = WaitState::Cancelled;
                (void)wheel_.cancel(id_);
                if (handle_)
                    wheel_.wake(handle_);
            });
        }
    }

    [[nodiscard]] auto await_resume() -> Result<Unit>
    {
        cancel_cb_.reset();
        if (state_ == WaitState::Cancelled) {
            return Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                     .message = "sleep cancelled",
                                     .location = std::source_location::current() });
        }
        return Ok();
    }

private:
    HierarchicalTimerWheel& wheel_;
    Duration delay_;
    TimerId id_ = 0;
    std::coroutine_handle<> handle_ {};
    WaitState state_ = WaitState::Pending;
    std::stop_token token_ {};
    std::optional<std::stop_callback<std::function<void()>>> cancel_cb_ {};
};

[[nodiscard]] inline auto sleep_for(HierarchicalTimerWheel& wheel, Duration delay) -> SleepAwaiter
{
    return SleepAwaiter { wheel, delay };
}

static_assert(TimerLike<HierarchicalTimerWheel>);
} // namespace rain::async
