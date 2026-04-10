#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/concepts.hpp"

#include <algorithm>
#include <array>
#include <coroutine>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rain::async {

// --- 定时器条目 ---

struct TimerEntry {
    TimerId id;
    TimePoint deadline;
    std::coroutine_handle<> handle;
    WaitState* state = nullptr;
    WaitState completion = WaitState::TimedOut;
};

// --- 层级 ---

struct Level {
    std::vector<std::vector<TimerEntry>> slots;
    usize current_slot = 0;

    Level() = default;
    explicit Level(usize num_slots) : slots(num_slots) { }

    [[nodiscard]] auto slot_count() const noexcept -> usize { return slots.size(); }
};

// --- 分层时间轮 ---

static constexpr usize kLevel0Bits = 8;
static constexpr usize kLevel0Size = 1u << kLevel0Bits; // 256
static constexpr usize kLevelNBits = 6;
static constexpr usize kLevelNSize = 1u << kLevelNBits; // 64
static constexpr usize kNumLevels = 4;

static constexpr Duration kTickDuration = std::chrono::milliseconds(1);

// insert_entry 路由使用的各层阈值（单位：tick）
static constexpr u64 kLevel1Threshold = kLevel0Size;                             // 256
static constexpr u64 kLevel2Threshold = kLevel0Size * kLevelNSize;               // 16,384
static constexpr u64 kLevel3Threshold = kLevel0Size * kLevelNSize * kLevelNSize; // 1,048,576

// 最大可表示延迟：约 18.6 小时
static constexpr Duration kMaxDelay
    = kTickDuration * static_cast<i64>(kLevel0Size * kLevelNSize * kLevelNSize * kLevelNSize);

class HierarchicalTimerWheel {
public:
    HierarchicalTimerWheel()
        : levels_ { Level(kLevel0Size), Level(kLevelNSize), Level(kLevelNSize), Level(kLevelNSize) },
          start_time_(Clock::now()), current_time_(start_time_)
    { }

    [[nodiscard]] auto schedule(Duration delay, std::coroutine_handle<> handle) -> TimerId
    {
        const auto deadline = current_time_ + delay;
        return schedule_at(deadline, handle);
    }

    [[nodiscard]] auto schedule(Duration delay,
                                std::coroutine_handle<> handle,
                                WaitState* state,
                                WaitState completion = WaitState::TimedOut) -> TimerId
    {
        const auto deadline = current_time_ + delay;
        return schedule_at(deadline, handle, state, completion);
    }

    [[nodiscard]] auto schedule_at(TimePoint deadline, std::coroutine_handle<> handle) -> TimerId
    {
        const auto id = next_id_++;
        insert_entry(TimerEntry { .id = id, .deadline = deadline, .handle = handle });
        return id;
    }

    [[nodiscard]] auto schedule_at(TimePoint deadline,
                                   std::coroutine_handle<> handle,
                                   WaitState* state,
                                   WaitState completion = WaitState::TimedOut) -> TimerId
    {
        const auto id = next_id_++;
        insert_entry(
            TimerEntry { .id = id, .deadline = deadline, .handle = handle, .state = state, .completion = completion });
        return id;
    }

    [[nodiscard]] auto cancel(TimerId id) -> bool
    {
        auto [_, inserted] = cancelled_.insert(id);
        return inserted;
    }

    [[nodiscard]] auto tick() -> u32
    {
        current_time_ += kTickDuration;
        return process_current_slot();
    }

    [[nodiscard]] auto advance_to(TimePoint target) -> u32
    {
        u32 total = 0;
        while (current_time_ < target) {
            total += tick();
        }
        return total;
    }

    [[nodiscard]] auto next_deadline() const -> std::optional<TimePoint>
    {
        const auto& level0 = levels_[0];
        for (usize i = 0; i < level0.slot_count(); ++i) {
            const usize slot = (level0.current_slot + i) % level0.slot_count();
            for (const auto& entry : level0.slots[slot]) {
                if (!cancelled_.contains(entry.id)) {
                    return entry.deadline;
                }
            }
        }

        for (usize lvl = 1; lvl < kNumLevels; ++lvl) {
            const auto& level = levels_[lvl];
            for (usize i = 0; i < level.slot_count(); ++i) {
                const usize slot = (level.current_slot + i) % level.slot_count();
                if (!level.slots[slot].empty()) {
                    std::optional<TimePoint> earliest;
                    for (const auto& entry : level.slots[slot]) {
                        if (!cancelled_.contains(entry.id)) {
                            if (!earliest || entry.deadline < *earliest) {
                                earliest = entry.deadline;
                            }
                        }
                    }
                    if (earliest)
                        return earliest;
                }
            }
        }

        return std::nullopt;
    }

    [[nodiscard]] auto drain_ready() -> std::vector<std::coroutine_handle<>> { return std::exchange(ready_, { }); }

    [[nodiscard]] auto current_time() const noexcept -> TimePoint { return current_time_; }

private:
    auto insert_entry(TimerEntry entry) -> Unit
    {
        using namespace std::chrono;
        const auto delay = entry.deadline - current_time_;
        auto ticks = duration_cast<milliseconds>(delay).count();

        if (ticks < 0)
            ticks = 0;

        const auto uticks = static_cast<u64>(ticks);

        if (uticks < kLevel1Threshold) {
            const usize slot = (levels_[0].current_slot + static_cast<usize>(uticks)) % kLevel0Size;
            levels_[0].slots[slot].push_back(std::move(entry));
        } else if (uticks < kLevel2Threshold) {
            const usize idx = static_cast<usize>(uticks >> kLevel0Bits);
            const usize slot = (levels_[1].current_slot + idx) % kLevelNSize;
            levels_[1].slots[slot].push_back(std::move(entry));
        } else if (uticks < kLevel3Threshold) {
            const usize idx = static_cast<usize>(uticks >> (kLevel0Bits + kLevelNBits));
            const usize slot = (levels_[2].current_slot + idx) % kLevelNSize;
            levels_[2].slots[slot].push_back(std::move(entry));
        } else {
            const usize idx = static_cast<usize>(uticks >> (kLevel0Bits + 2 * kLevelNBits));
            const usize slot = (levels_[3].current_slot + std::min(idx, kLevelNSize - 1)) % kLevelNSize;
            levels_[3].slots[slot].push_back(std::move(entry));
        }
        return unit;
    }

    [[nodiscard]] auto process_current_slot() -> u32
    {
        if (levels_[0].current_slot == 0) {
            cascade(1);
        }

        auto& slot = levels_[0].slots[levels_[0].current_slot];
        u32 fired = 0;

        for (auto& entry : slot) {
            if (cancelled_.contains(entry.id)) {
                cancelled_.erase(entry.id);
                continue;
            }
            if (entry.state) {
                if (*entry.state != WaitState::Pending) {
                    continue;
                }
                *entry.state = entry.completion;
            }
            if (entry.handle && !entry.handle.done()) {
                ready_.push_back(entry.handle);
                ++fired;
            }
        }
        slot.clear();

        levels_[0].current_slot = (levels_[0].current_slot + 1) % kLevel0Size;
        return fired;
    }

    auto cascade(usize level) -> Unit
    {
        if (level >= kNumLevels)
            return unit;

        if (levels_[level].current_slot == 0 && level + 1 < kNumLevels) {
            cascade(level + 1);
        }

        auto entries = std::exchange(levels_[level].slots[levels_[level].current_slot], { });
        for (auto& entry : entries) {
            if (cancelled_.contains(entry.id)) {
                cancelled_.erase(entry.id);
                continue;
            }
            insert_entry(std::move(entry));
        }

        levels_[level].current_slot = (levels_[level].current_slot + 1) % levels_[level].slot_count();
        return unit;
    }

    std::array<Level, kNumLevels> levels_;
    TimePoint start_time_;
    TimePoint current_time_;
    u64 next_id_ = 1;
    std::unordered_set<TimerId> cancelled_;
    std::vector<std::coroutine_handle<>> ready_;
};

// --- 睡眠等待器（SleepAwaiter）---

class SleepAwaiter {
public:
    SleepAwaiter(HierarchicalTimerWheel& wheel, Duration delay) : wheel_(wheel), delay_(delay) { }

    [[nodiscard]] auto await_ready() const noexcept -> bool { return delay_ <= Duration::zero(); }

    void await_suspend(std::coroutine_handle<> h) { id_ = wheel_.schedule(delay_, h); }

    [[nodiscard]] auto await_resume() -> Result<Unit> { return Ok(); }

private:
    HierarchicalTimerWheel& wheel_;
    Duration delay_;
    TimerId id_ = 0;
};

[[nodiscard]] inline auto sleep_for(HierarchicalTimerWheel& wheel, Duration delay) -> SleepAwaiter
{
    return SleepAwaiter { wheel, delay };
}

// 校验概念符合性
static_assert(TimerLike<HierarchicalTimerWheel>);

} // namespace rain::async
