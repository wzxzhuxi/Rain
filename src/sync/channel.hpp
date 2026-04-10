#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

namespace rain::sync {

template<typename T>
class Channel {
public:
    explicit Channel(usize capacity = 0) : capacity_(capacity) { }

    [[nodiscard]] auto send(T value) -> Result<Unit>
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (capacity_ > 0) {
            not_full_.wait(lock, [this] { return queue_.size() < capacity_ || closed_; });
        }
        if (closed_) {
            return Err(SystemError { .code = std::make_error_code(std::errc::broken_pipe),
                                     .message = "send on closed channel",
                                     .location = std::source_location::current() });
        }
        queue_.push(std::move(value));
        not_empty_.notify_one();
        return Ok();
    }

    [[nodiscard]] auto try_send(T value) -> Result<Unit>
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return Err(SystemError { .code = std::make_error_code(std::errc::broken_pipe),
                                     .message = "send on closed channel",
                                     .location = std::source_location::current() });
        }
        if (capacity_ > 0 && queue_.size() >= capacity_) {
            return Err(SystemError { .code = std::make_error_code(std::errc::resource_unavailable_try_again),
                                     .message = "send on full channel",
                                     .location = std::source_location::current() });
        }
        queue_.push(std::move(value));
        not_empty_.notify_one();
        return Ok();
    }

    [[nodiscard]] auto recv() -> std::optional<T>
    {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop();
        not_full_.notify_one();
        return value;
    }
    [[nodiscard]] auto try_recv() -> std::optional<T>
    {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop();
        not_full_.notify_one();
        return value;
    }
    template<typename Rep, typename Period>
    [[nodiscard]] auto recv_timeout(std::chrono::duration<Rep, Period> timeout) -> std::optional<T>
    {
        std::unique_lock lock(mutex_);
        if (!not_empty_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; })) {
            return std::nullopt;
        }
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop();
        not_full_.notify_one();
        return value;
    }
    auto close() -> Unit
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
        return unit;
    }
    [[nodiscard]] auto is_closed() const -> bool
    {
        std::lock_guard lock(mutex_);
        return closed_;
    }
    [[nodiscard]] auto size() const -> usize
    {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::queue<T> queue_;
    usize capacity_;
    bool closed_ = false;
};
template<typename T>
[[nodiscard]] auto make_channel(usize capacity = 0) -> Channel<T>
{
    return Channel<T>(capacity);
}
} // namespace rain::sync
