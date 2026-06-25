#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/concepts.hpp"

#include <array>
#include <coroutine>
#include <functional>
#include <optional>
#include <stop_token>
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

// place 路由使用的各层阈值（单位：tick）
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
        return schedule_at(deadline, handle, nullptr, WaitState::TimedOut);
    }

    [[nodiscard]] auto schedule_at(TimePoint deadline,
                                   std::coroutine_handle<> handle,
                                   WaitState* state,
                                   WaitState completion = WaitState::TimedOut) -> TimerId
    {
        // 从空闲栈取一个稠密下标（或追加），写入 entry，以 (index, generation) 编码 TimerId。
        u32 index;
        if (!free_indices_.empty()) {
            index = free_indices_.back();
            free_indices_.pop_back();
        } else {
            index = static_cast<u32>(slotmap_.size());
            slotmap_.push_back({ });
        }
        auto& s = slotmap_[index];
        ++s.generation; // 新一代：与该下标曾经的占用者区分（解决 ABA）
        s.active = true;
        const auto id = encode_id(index, s.generation);
        s.entry
            = TimerEntry { .id = id, .deadline = deadline, .handle = handle, .state = state, .completion = completion };
        ++active_count_;
        place(id, deadline);
        return id;
    }

    // 立即从条目表删除——O(1) 急切取消，不再留墓碑。槽位中残留的 id
    // 在扫描时查不到条目即跳过，因此也不会再解引用已销毁的 state 指针。
    [[nodiscard]] auto cancel(TimerId id) -> bool
    {
        const auto idx = id_index(id);
        if (idx >= slotmap_.size())
            return false;
        auto& s = slotmap_[idx];
        if (!s.active || s.generation != id_gen(id))
            return false;
        s.active = false;
        free_indices_.push_back(idx);
        --active_count_;
        return true;
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
        // 无活跃定时器:直接 O(1) 返回,免去每轮事件循环白扫全部 448 个槽。
        if (active_count_ == 0)
            return std::nullopt;

        const auto& level0 = levels_[0];
        for (usize i = 0; i < level0.slot_count(); ++i) {
            const usize slot = (level0.current_slot + i) % level0.slot_count();
            for (const auto id : level0.slots[slot]) {
                if (const auto* entry = lookup(id)) {
                    return entry->deadline;
                }
            }
        }

        for (usize lvl = 1; lvl < kNumLevels; ++lvl) {
            const auto& level = levels_[lvl];
            for (usize i = 0; i < level.slot_count(); ++i) {
                const usize slot = (level.current_slot + i) % level.slot_count();
                std::optional<TimePoint> earliest;
                for (const auto id : level.slots[slot]) {
                    if (const auto* entry = lookup(id)) {
                        if (!earliest || entry->deadline < *earliest) {
                            earliest = entry->deadline;
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

    // 追加到 out 后清空 ready_（保留容量复用），避免每轮 drain 一个中间 vector。
    auto drain_ready_into(std::vector<std::coroutine_handle<>>& out) -> void
    {
        out.insert(out.end(), ready_.begin(), ready_.end());
        ready_.clear();
    }

    [[nodiscard]] auto current_time() const noexcept -> TimePoint { return current_time_; }

    // 把一个句柄直接放进就绪队列。供 SleepAwaiter 取消时唤醒挂起的协程——timer 层不能反依赖
    // event_loop（会成环），故借 wheel 自己的 ready_（事件循环每轮经 drain_ready_into 排空它）。
    auto wake(std::coroutine_handle<> h) -> void { ready_.push_back(h); }

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

    // TimerId 编码：低 24 位为稠密下标（上限 1600 万并发 timer，远超任何实际场景），
    // 高 40 位为代号。40 位代号意味着同一下标需复用约 1.1 万亿次才回绕——实践不可达，
    // 据此彻底杜绝 generation 回绕导致的 ABA（一个已取消、残留在远期槽里的旧 id 被错误复活）。
    // 注：LIFO 空闲栈会把复用集中在最热的下标，这正是最快逼近回绕的路径，故选 40 位留足余量。
    static constexpr u32 kIndexBits = 24;
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1;
    static constexpr auto encode_id(u32 index, u64 gen) -> TimerId { return (gen << kIndexBits) | index; }
    static constexpr auto id_index(TimerId id) -> u32 { return static_cast<u32>(id & kIndexMask); }
    static constexpr auto id_gen(TimerId id) -> u64 { return id >> kIndexBits; }

    // 以 id 直接索引 slotmap_（0 哈希），校验 active + generation；无效（已取消/已触发/stale）返回 nullptr。
    [[nodiscard]] auto lookup(TimerId id) const -> const TimerEntry*
    {
        const auto idx = id_index(id);
        if (idx >= slotmap_.size())
            return nullptr;
        const auto& s = slotmap_[idx];
        if (!s.active || s.generation != id_gen(id))
            return nullptr;
        return &s.entry;
    }

    // 回收某 id 占用的下标（取消/触发后）：标记空闲并入空闲栈。对已失效的 id 幂等。
    auto release(TimerId id) -> void
    {
        const auto idx = id_index(id);
        if (idx < slotmap_.size() && slotmap_[idx].active && slotmap_[idx].generation == id_gen(id)) {
            slotmap_[idx].active = false;
            free_indices_.push_back(idx);
            --active_count_;
        }
    }

    [[nodiscard]] auto process_current_slot() -> u32
    {
        if (levels_[0].current_slot == 0) {
            cascade(1);
        }

        auto slot = std::exchange(levels_[0].slots[levels_[0].current_slot], { });
        u32 fired = 0;

        for (const auto id : slot) {
            const auto* entry = lookup(id);
            if (!entry) {
                continue; // 已取消或已触发
            }
            if (entry->state) {
                if (*entry->state != WaitState::Pending) {
                    release(id);
                    continue;
                }
                *entry->state = entry->completion;
            }
            if (entry->handle && !entry->handle.done()) {
                ready_.push_back(entry->handle);
                ++fired;
            }
            release(id);
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
            const auto* entry = lookup(id);
            if (!entry) {
                continue; // 已取消
            }
            place(id, entry->deadline);
        }

        levels_[level].current_slot = (levels_[level].current_slot + 1) % levels_[level].slot_count();
        return unit;
    }

    // 稠密 slotmap：以 TimerId 高位的 index 直接寻址，取代 unordered_map<TimerId, TimerEntry>。
    // 0 哈希、0 节点分配（下标从空闲栈复用）；generation 区分同一下标的历代占用者（ABA）。
    struct Slot {
        TimerEntry entry;
        u64 generation = 0; // 当前占用者代号（高 40 位编入 TimerId）；与 id 的代号匹配才有效
        bool active = false;
    };

    std::array<Level, kNumLevels> levels_;
    TimePoint start_time_;
    TimePoint current_time_;
    std::vector<Slot> slotmap_;     // 稠密条目表，按 index
    std::vector<u32> free_indices_; // 空闲下标栈（LIFO 复用）
    usize active_count_ = 0;        // 活跃定时器数，供 next_deadline 的 O(1) 空判断
    std::vector<std::coroutine_handle<>> ready_;
};

// --- 睡眠等待器（SleepAwaiter）---

class SleepAwaiter {
public:
    SleepAwaiter(HierarchicalTimerWheel& wheel, Duration delay) : wheel_(wheel), delay_(delay) { }

    // 注入取消令牌：sleep 在被取消（如 timeout 中 task 先完成）时立即唤醒，不再悬挂 timer。
    auto set_cancellation(std::stop_token token) -> void { token_ = std::move(token); }

    [[nodiscard]] auto await_ready() const noexcept -> bool { return delay_ <= Duration::zero(); }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle_ = h;
        // 带 state 排程：fire 做 Pending→Ready，与取消回调的 Pending→Cancelled 竞争（owner 核串行）。
        id_ = wheel_.schedule(delay_, h, &state_, WaitState::Ready);
        if (token_.stop_possible())
            cancel_cb_.emplace(token_, [this] {
                if (state_ != WaitState::Pending)
                    return;
                state_ = WaitState::Cancelled;
                (void)wheel_.cancel(id_);
                if (handle_)
                    wheel_.wake(handle_);
            });
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

// 校验概念符合性
static_assert(TimerLike<HierarchicalTimerWheel>);

} // namespace rain::async
