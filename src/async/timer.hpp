#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/concepts.hpp"

#include <array>
#include <coroutine>
#include <optional>
#include <unordered_map>
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
    std::vector<std::vector<TimerId>> slots;
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

    // 立即从条目表删除——O(1) 急切取消，不再留墓碑。槽位中残留的 id
    // 在扫描时查不到条目即跳过，因此也不会再解引用已销毁的 state 指针。
    [[nodiscard]] auto cancel(TimerId id) -> bool
    {
        return entries_.erase(id) > 0;
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
            for (const auto id : level0.slots[slot]) {
                auto it = entries_.find(id);
                if (it != entries_.end()) {
                    return it->second.deadline;
                }
            }
        }

        for (usize lvl = 1; lvl < kNumLevels; ++lvl) {
            const auto& level = levels_[lvl];
            for (usize i = 0; i < level.slot_count(); ++i) {
                const usize slot = (level.current_slot + i) % level.slot_count();
                std::optional<TimePoint> earliest;
                for (const auto id : level.slots[slot]) {
                    auto it = entries_.find(id);
                    if (it != entries_.end()) {
                        if (!earliest || it->second.deadline < *earliest) {
                            earliest = it->second.deadline;
                        }
                    }
                }
                if (earliest)
                    return earliest;
            }
        }

        return std::nullopt;
    }

    [[nodiscard]] auto drain_ready() -> std::vector<std::coroutine_handle<>> { return std::exchange(ready_, { }); }

    [[nodiscard]] auto current_time() const noexcept -> TimePoint { return current_time_; }

private:
    // 按 deadline 的绝对 tick 位段把 id 放入对应层级的槽位（各层 current_slot 同样是
    // 绝对时间位段），否则级联下放时相位不对齐，会导致定时器晚触发整整一圈。
    auto place(TimerId id, TimePoint deadline) -> Unit
    {
        using namespace std::chrono;
        auto delay = duration_cast<milliseconds>(deadline - current_time_).count();
        if (delay < 0)
            delay = 0;
        const auto diff = static_cast<u64>(delay);

        auto abs = duration_cast<milliseconds>(deadline - start_time_).count();
        if (abs < 0)
            abs = 0;
        const auto dticks = static_cast<u64>(abs);

        if (diff < kLevel1Threshold) {
            levels_[0].slots[dticks & (kLevel0Size - 1)].push_back(id);
        } else if (diff < kLevel2Threshold) {
            levels_[1].slots[(dticks >> kLevel0Bits) & (kLevelNSize - 1)].push_back(id);
        } else if (diff < kLevel3Threshold) {
            levels_[2].slots[(dticks >> (kLevel0Bits + kLevelNBits)) & (kLevelNSize - 1)].push_back(id);
        } else {
            levels_[3].slots[(dticks >> (kLevel0Bits + 2 * kLevelNBits)) & (kLevelNSize - 1)].push_back(id);
        }
        return unit;
    }

    // 存储条目（条目表是唯一真相），并把其 id 放入槽位。
    auto insert_entry(TimerEntry entry) -> Unit
    {
        const auto id = entry.id;
        const auto deadline = entry.deadline;
        entries_.insert_or_assign(id, entry);
        place(id, deadline);
        return unit;
    }

    [[nodiscard]] auto process_current_slot() -> u32
    {
        if (levels_[0].current_slot == 0) {
            cascade(1);
        }

        auto slot = std::exchange(levels_[0].slots[levels_[0].current_slot], { });
        u32 fired = 0;

        for (const auto id : slot) {
            auto it = entries_.find(id);
            if (it == entries_.end()) {
                continue; // 已取消或已触发
            }
            auto& entry = it->second;
            if (entry.state) {
                if (*entry.state != WaitState::Pending) {
                    entries_.erase(it);
                    continue;
                }
                *entry.state = entry.completion;
            }
            if (entry.handle && !entry.handle.done()) {
                ready_.push_back(entry.handle);
                ++fired;
            }
            entries_.erase(it);
        }

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

        auto ids = std::exchange(levels_[level].slots[levels_[level].current_slot], { });
        for (const auto id : ids) {
            auto it = entries_.find(id);
            if (it == entries_.end()) {
                continue; // 已取消
            }
            place(id, it->second.deadline);
        }

        levels_[level].current_slot = (levels_[level].current_slot + 1) % levels_[level].slot_count();
        return unit;
    }

    std::array<Level, kNumLevels> levels_;
    TimePoint start_time_;
    TimePoint current_time_;
    u64 next_id_ = 1;
    std::unordered_map<TimerId, TimerEntry> entries_;
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
