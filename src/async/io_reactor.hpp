#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/concepts.hpp"

#include <cerrno>
#include <chrono>
#include <coroutine>
#include <optional>
#include <sys/epoll.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rain::async {

namespace detail {

[[nodiscard]] constexpr auto to_epoll_events(IoEvent ev) -> u32
{
    u32 result = 0;
    if (has_flag(ev, IoEvent::Read))
        result |= EPOLLIN;
    if (has_flag(ev, IoEvent::Write))
        result |= EPOLLOUT;
    if (has_flag(ev, IoEvent::Error))
        result |= EPOLLERR;
    if (has_flag(ev, IoEvent::HangUp))
        result |= EPOLLHUP;
    if (has_flag(ev, IoEvent::EdgeTrig))
        result |= EPOLLET;
    if (has_flag(ev, IoEvent::OneShot))
        result |= EPOLLONESHOT;
    return result;
}

} // namespace detail

class ReadAwaiter;
class WriteAwaiter;

class EpollReactor {
public:
    static constexpr usize kMaxEventsPerPoll = 256;

    EpollReactor(const EpollReactor&) = delete;
    auto operator=(const EpollReactor&) -> EpollReactor& = delete;

    EpollReactor(EpollReactor&& other) noexcept
        : epoll_fd_(std::exchange(other.epoll_fd_, -1)), events_(std::move(other.events_)),
          pending_(std::move(other.pending_)), ready_(std::move(other.ready_))
    { }

    auto operator=(EpollReactor&& other) noexcept -> EpollReactor&
    {
        if (this != &other) {
            if (auto close_result = close(); !close_result) {
                // 移动赋值清理路径仅做 best-effort。
            }
            epoll_fd_ = std::exchange(other.epoll_fd_, -1);
            events_ = std::move(other.events_);
            pending_ = std::move(other.pending_);
            ready_ = std::move(other.ready_);
        }
        return *this;
    }

    ~EpollReactor()
    {
        if (auto close_result = close(); !close_result) {
            // 析构路径仅做 best-effort。
        }
    }

    [[nodiscard]] static auto create() -> Result<EpollReactor>
    {
        const i32 fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (fd < 0)
            return Err(SystemError::from_errno());
        return Ok(EpollReactor(fd));
    }

    [[nodiscard]] auto add(i32 fd, IoEvent events) -> Result<Unit>
    {
        struct epoll_event ev { };
        ev.events = detail::to_epoll_events(events);
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            return Err(SystemError::from_errno());
        }
        return Ok();
    }

    [[nodiscard]] auto modify(i32 fd, IoEvent events) -> Result<Unit>
    {
        struct epoll_event ev { };
        ev.events = detail::to_epoll_events(events);
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
            return Err(SystemError::from_errno());
        }
        return Ok();
    }

    [[nodiscard]] auto remove(i32 fd) -> Result<Unit>
    {
        if (fd < 0) {
            return Ok();
        }
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
            if (errno != ENOENT && errno != EBADF) {
                return Err(SystemError::from_errno());
            }
        }
        pending_.erase(fd);
        return Ok();
    }

    [[nodiscard]] auto poll(Duration timeout) -> Result<u32>
    {
        using namespace std::chrono;
        const auto ms = duration_cast<milliseconds>(timeout).count();
        const i32 timeout_ms = static_cast<i32>(ms < 0 ? -1 : ms);

        const i32 n = ::epoll_wait(epoll_fd_, events_.data(), static_cast<i32>(events_.size()), timeout_ms);

        if (n < 0) {
            if (errno == EINTR)
                return Ok(u32 { 0 });
            return Err(SystemError::from_errno());
        }

        u32 completed = 0;
        for (i32 i = 0; i < n; ++i) {
            const i32 fd = events_[static_cast<usize>(i)].data.fd;
            const u32 ep = events_[static_cast<usize>(i)].events;

            auto it = pending_.find(fd);
            if (it == pending_.end())
                continue;

            auto entry = it->second;
            pending_.erase(it);

            (void)ep;
            if (entry.state) {
                if (*entry.state != WaitState::Pending) {
                    continue;
                }
                *entry.state = entry.completion;
            }
            if (entry.result) {
                *entry.result = Ok(usize { 0 });
            }

            if (entry.handle) {
                ready_.push_back(entry.handle);
                ++completed;
            }
        }

        return Ok(completed);
    }

    [[nodiscard]] auto
    register_read(i32 fd, std::coroutine_handle<> h, u8* /*buf*/, usize /*len*/, Result<usize>* result_ptr)
        -> Result<Unit>
    {
        return register_read(fd, h, nullptr, 0, result_ptr, nullptr);
    }

    [[nodiscard]] auto register_read(i32 fd,
                                     std::coroutine_handle<> h,
                                     u8* /*buf*/,
                                     usize /*len*/,
                                     Result<usize>* result_ptr,
                                     WaitState* state,
                                     WaitState completion = WaitState::Ready) -> Result<Unit>
    {
        if (fd < 0) {
            return Err(SystemError { .code = std::make_error_code(std::errc::bad_file_descriptor),
                                     .message = "register_read: negative fd",
                                     .location = std::source_location::current() });
        }
        if (pending_.contains(fd)) {
            return Err(SystemError { .code = std::make_error_code(std::errc::device_or_resource_busy),
                                     .message = "register_read: fd already pending",
                                     .location = std::source_location::current() });
        }

        pending_.emplace(fd,
                         PendingEntry { .handle = h, .result = result_ptr, .state = state, .completion = completion });

        auto ensure_result = ensure(fd, IoEvent::Read | IoEvent::EdgeTrig | IoEvent::OneShot);
        if (!ensure_result) {
            pending_.erase(fd);
            return Err(std::move(ensure_result).error());
        }
        return Ok();
    }

    [[nodiscard]] auto
    register_write(i32 fd, std::coroutine_handle<> h, const u8* /*buf*/, usize /*len*/, Result<usize>* result_ptr)
        -> Result<Unit>
    {
        return register_write(fd, h, nullptr, 0, result_ptr, nullptr);
    }

    [[nodiscard]] auto register_write(i32 fd,
                                      std::coroutine_handle<> h,
                                      const u8* /*buf*/,
                                      usize /*len*/,
                                      Result<usize>* result_ptr,
                                      WaitState* state,
                                      WaitState completion = WaitState::Ready) -> Result<Unit>
    {
        if (fd < 0) {
            return Err(SystemError { .code = std::make_error_code(std::errc::bad_file_descriptor),
                                     .message = "register_write: negative fd",
                                     .location = std::source_location::current() });
        }
        if (pending_.contains(fd)) {
            return Err(SystemError { .code = std::make_error_code(std::errc::device_or_resource_busy),
                                     .message = "register_write: fd already pending",
                                     .location = std::source_location::current() });
        }

        pending_.emplace(fd,
                         PendingEntry { .handle = h, .result = result_ptr, .state = state, .completion = completion });

        auto ensure_result = ensure(fd, IoEvent::Write | IoEvent::EdgeTrig | IoEvent::OneShot);
        if (!ensure_result) {
            pending_.erase(fd);
            return Err(std::move(ensure_result).error());
        }
        return Ok();
    }

    auto deregister(i32 fd) -> Unit
    {
        pending_.erase(fd);
        return unit;
    }

    [[nodiscard]] auto drain_ready() -> std::vector<std::coroutine_handle<>> { return std::exchange(ready_, { }); }

    [[nodiscard]] auto valid() const noexcept -> bool { return epoll_fd_ >= 0; }

    [[nodiscard]] auto close() -> Result<Unit>
    {
        std::optional<SystemError> first_error;
        if (epoll_fd_ >= 0) {
            if (::close(epoll_fd_) < 0) {
                first_error = SystemError::from_errno();
            }
            epoll_fd_ = -1;
        }
        pending_.clear();
        ready_.clear();
        if (first_error.has_value()) {
            return Err(std::move(*first_error));
        }
        return Ok();
    }

    [[nodiscard]] inline auto async_read(i32 fd, u8* buf, usize len) -> ReadAwaiter;
    [[nodiscard]] inline auto async_write(i32 fd, const u8* buf, usize len) -> WriteAwaiter;

private:
    struct PendingEntry {
        std::coroutine_handle<> handle { };
        Result<usize>* result = nullptr;
        WaitState* state = nullptr;
        WaitState completion = WaitState::Ready;
    };

    explicit EpollReactor(i32 epoll_fd) : epoll_fd_(epoll_fd), events_(kMaxEventsPerPoll) { }

    [[nodiscard]] auto ensure(i32 fd, IoEvent events) -> Result<Unit>
    {
        struct epoll_event ev { };
        ev.events = detail::to_epoll_events(events);
        ev.data.fd = fd;

        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0) {
            return Ok();
        }
        if (errno == ENOENT) {
            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
                return Err(SystemError::from_errno());
            }
            return Ok();
        }
        return Err(SystemError::from_errno());
    }

    i32 epoll_fd_ = -1;
    std::vector<epoll_event> events_;
    std::unordered_map<i32, PendingEntry> pending_;
    std::vector<std::coroutine_handle<>> ready_;
};

static_assert(IoReactorLike<EpollReactor>);

class ReadAwaiter {
public:
    ReadAwaiter(EpollReactor& reactor, i32 fd, u8* buf, usize len)
        : reactor_(reactor), fd_(fd), buf_(buf), len_(len) { }

    [[nodiscard]] auto await_ready() noexcept -> bool
    {
        const auto bytes = ::read(fd_, buf_, len_);
        if (bytes > 0) {
            result_ = Ok(static_cast<usize>(bytes));
            immediate_ = true;
            return true;
        }
        if (bytes == 0) {
            result_ = Ok(usize { 0 });
            immediate_ = true;
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }
        result_ = Err(SystemError::from_errno());
        immediate_ = true;
        return true;
    }

    auto await_suspend(std::coroutine_handle<> h) -> bool
    {
        auto reg = reactor_.register_read(fd_, h, nullptr, 0, &readiness_);
        if (!reg) {
            readiness_ = Err(std::move(reg).error());
            return false;
        }
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<usize>
    {
        if (immediate_) {
            return std::move(result_);
        }
        if (!readiness_) {
            return Err(std::move(readiness_).error());
        }

        while (true) {
            const auto bytes = ::read(fd_, buf_, len_);
            if (bytes >= 0) {
                return Ok(static_cast<usize>(bytes));
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return Err(SystemError { .code = std::make_error_code(std::errc::resource_unavailable_try_again),
                                         .message = "read would block after readiness notification",
                                         .location = std::source_location::current() });
            }
            return Err(SystemError::from_errno());
        }
    }

private:
    EpollReactor& reactor_;
    i32 fd_;
    u8* buf_;
    usize len_;
    bool immediate_ = false;
    Result<usize> result_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                              .message = "read not completed",
                                              .location = std::source_location::current() });
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "read readiness not completed",
                                                 .location = std::source_location::current() });
};

class WriteAwaiter {
public:
    WriteAwaiter(EpollReactor& reactor, i32 fd, const u8* buf, usize len)
        : reactor_(reactor), fd_(fd), buf_(buf), len_(len)
    { }

    [[nodiscard]] auto await_ready() noexcept -> bool
    {
        const auto bytes = ::write(fd_, buf_, len_);
        if (bytes >= 0) {
            result_ = Ok(static_cast<usize>(bytes));
            immediate_ = true;
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }
        result_ = Err(SystemError::from_errno());
        immediate_ = true;
        return true;
    }

    auto await_suspend(std::coroutine_handle<> h) -> bool
    {
        auto reg = reactor_.register_write(fd_, h, nullptr, 0, &readiness_);
        if (!reg) {
            readiness_ = Err(std::move(reg).error());
            return false;
        }
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<usize>
    {
        if (immediate_) {
            return std::move(result_);
        }
        if (!readiness_) {
            return Err(std::move(readiness_).error());
        }

        while (true) {
            const auto bytes = ::write(fd_, buf_, len_);
            if (bytes >= 0) {
                return Ok(static_cast<usize>(bytes));
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return Err(SystemError { .code = std::make_error_code(std::errc::resource_unavailable_try_again),
                                         .message = "write would block after readiness notification",
                                         .location = std::source_location::current() });
            }
            return Err(SystemError::from_errno());
        }
    }

private:
    EpollReactor& reactor_;
    i32 fd_;
    const u8* buf_;
    usize len_;
    bool immediate_ = false;
    Result<usize> result_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                              .message = "write not completed",
                                              .location = std::source_location::current() });
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "write readiness not completed",
                                                 .location = std::source_location::current() });
};

[[nodiscard]] inline auto EpollReactor::async_read(i32 fd, u8* buf, usize len) -> ReadAwaiter
{
    return ReadAwaiter { *this, fd, buf, len };
}

[[nodiscard]] inline auto EpollReactor::async_write(i32 fd, const u8* buf, usize len) -> WriteAwaiter
{
    return WriteAwaiter { *this, fd, buf, len };
}

} // namespace rain::async
