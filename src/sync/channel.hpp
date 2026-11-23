#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <vector>

namespace rain::sync {

// 无锁有界 MPMC 队列（Vyukov）：入队/出队只用原子 CAS 与 cell 序号，
// 数据通路不加锁；mutex+cv 仅用于“满/空时阻塞等待”，不参与数据传递。
template<typename T>
class alignas(64) Channel {
public:
    // capacity == 0 为兼容旧 API 的默认值，映射为 kDefaultCapacity（有界）。
    explicit Channel(usize capacity = 0)
        : capacity_(capacity == 0 ? kDefaultCapacity : capacity), cells_(capacity_)
    {
        for (usize i = 0; i < capacity_; ++i)
            cells_[i].seq.store(i, std::memory_order_relaxed);
    }

    Channel(const Channel&) = delete;
    auto operator=(const Channel&) -> Channel& = delete;

    [[nodiscard]] auto send(T value) -> Result<Unit>
    {
        if (closed_.load(std::memory_order_acquire))
            return closed_error("send on closed channel");
        if (try_push(std::move(value))) {
            notify(not_empty_);
            return Ok();
        }

        std::unique_lock lock(wait_mutex_);
        while (true) {
            if (closed_.load(std::memory_order_acquire))
                return closed_error("send on closed channel");
            if (try_push(std::move(value))) {
                not_empty_.notify_one();
                return Ok();
            }
            not_full_.wait(lock);
        }
    }

    [[nodiscard]] auto try_send(T value) -> Result<Unit>
    {
        if (closed_.load(std::memory_order_acquire))
            return closed_error("send on closed channel");
        if (!try_push(std::move(value)))
            return full_error();
        notify(not_empty_);
        return Ok();
    }

    [[nodiscard]] auto recv() -> std::optional<T>
    {
        if (auto value = try_pop()) {
            notify(not_full_);
            return value;
        }

        std::unique_lock lock(wait_mutex_);
        while (true) {
            if (auto value = try_pop()) {
                not_full_.notify_one();
                return value;
            }
            if (closed_.load(std::memory_order_acquire))
                return std::nullopt;
            not_empty_.wait(lock);
        }
    }

    [[nodiscard]] auto try_recv() -> std::optional<T>
    {
        auto value = try_pop();
        if (value)
            notify(not_full_);
        return value;
    }

    template<typename Rep, typename Period>
    [[nodiscard]] auto recv_timeout(std::chrono::duration<Rep, Period> timeout) -> std::optional<T>
    {
        std::unique_lock lock(wait_mutex_);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            if (auto value = try_pop()) {
                not_full_.notify_one();
                return value;
            }
            if (closed_.load(std::memory_order_acquire))
                return std::nullopt;
            if (not_empty_.wait_until(lock, deadline) == std::cv_status::timeout) {
                if (auto value = try_pop()) {
                    not_full_.notify_one();
                    return value;
                }
                return std::nullopt;
            }
        }
    }

    auto close() -> Unit
    {
        closed_.store(true, std::memory_order_release);
        {
            // 空锁段：让已通过条件检查、尚未进入 wait 的线程先落锁，避免丢唤醒。
            std::lock_guard lock(wait_mutex_);
        }
        not_empty_.notify_all();
        not_full_.notify_all();
        return unit;
    }

    [[nodiscard]] auto is_closed() const -> bool { return closed_.load(std::memory_order_acquire); }
    [[nodiscard]] auto size() const -> usize { return size_.load(std::memory_order_relaxed); }

private:
    struct Cell {
        std::atomic<usize> seq { 0 };
        std::optional<T> data;
    };

    static constexpr usize kDefaultCapacity = 1024;

    static auto closed_error(const char* msg) -> Result<Unit>
    {
        return Err(SystemError { .code = std::make_error_code(std::errc::broken_pipe),
                                 .message = msg,
                                 .location = std::source_location::current() });
    }

    static auto full_error() -> Result<Unit>
    {
        return Err(SystemError { .code = std::make_error_code(std::errc::resource_unavailable_try_again),
                                 .message = "send on full channel",
                                 .location = std::source_location::current() });
    }

    // 入队：CAS 推进 enqueue_pos，成功后写入元素并发布序号。失败（满）时不移动 value。
    auto try_push(T&& value) -> bool
    {
        usize pos = enqueue_pos_.load(std::memory_order_relaxed);
        while (true) {
            Cell& cell = cells_[pos % capacity_];
            const usize seq = cell.seq.load(std::memory_order_acquire);
            const auto dif = static_cast<isize>(seq) - static_cast<isize>(pos);
            if (dif == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    cell.data.emplace(std::move(value));
                    cell.seq.store(pos + 1, std::memory_order_release);
                    size_.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            } else if (dif < 0) {
                return false; // 满
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    // 出队：CAS 推进 dequeue_pos，取走元素并把 cell 序号重置为下一轮可用。
    auto try_pop() -> std::optional<T>
    {
        usize pos = dequeue_pos_.load(std::memory_order_relaxed);
        while (true) {
            Cell& cell = cells_[pos % capacity_];
            const usize seq = cell.seq.load(std::memory_order_acquire);
            const auto dif = static_cast<isize>(seq) - static_cast<isize>(pos + 1);
            if (dif == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    T value = std::move(*cell.data);
                    cell.data.reset();
                    cell.seq.store(pos + capacity_, std::memory_order_release);
                    size_.fetch_sub(1, std::memory_order_relaxed);
                    return value;
                }
            } else if (dif < 0) {
                return std::nullopt; // 空
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    // 空锁段 + notify：与 wait 侧配对，避免“检查后、入睡前”的唤醒丢失。
    auto notify(std::condition_variable& cv) -> void
    {
        {
            std::lock_guard lock(wait_mutex_);
        }
        cv.notify_one();
    }

    usize capacity_;
    std::vector<Cell> cells_;
    alignas(64) std::atomic<usize> enqueue_pos_ { 0 };
    alignas(64) std::atomic<usize> dequeue_pos_ { 0 };
    std::atomic<usize> size_ { 0 };
    std::atomic<bool> closed_ { false };
    mutable std::mutex wait_mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
};

template<typename T>
[[nodiscard]] auto make_channel(usize capacity = 0) -> Channel<T>
{
    return Channel<T>(capacity);
}

} // namespace rain::sync
