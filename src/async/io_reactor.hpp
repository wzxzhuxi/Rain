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

// epoll 封装：fd 作下标的稠密槽位表，0 哈希、0 节点分配。
// 数据 fd 首次使用时以 Read|Write|EdgeTrig 持久注册；就绪不再重新武装，
// 等待者只维护槽位，取消清槽，关闭才 DEL。
class EpollReactor {
public:
    static constexpr usize kMaxEventsPerPoll = 256;
    EpollReactor(const EpollReactor&) = delete;
    auto operator=(const EpollReactor&) -> EpollReactor& = delete;

    EpollReactor(EpollReactor&& other) noexcept
        : epoll_fd_(std::exchange(other.epoll_fd_, -1)), events_(std::move(other.events_)),
          slots_(std::move(other.slots_)), ready_(std::move(other.ready_))
    { }

    auto operator=(EpollReactor&& other) noexcept -> EpollReactor&
    {
        if (this != &other) {
            (void)close();
            epoll_fd_ = std::exchange(other.epoll_fd_, -1);
            events_ = std::move(other.events_);
            slots_ = std::move(other.slots_);
            ready_ = std::move(other.ready_);
        }
        return *this;
    }

    ~EpollReactor() { (void)close(); }

    [[nodiscard]] static auto create() -> Result<EpollReactor>
    {
        const i32 fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (fd < 0)
            return Err(SystemError::from_errno());
        return Ok(EpollReactor(fd));
    }

    [[nodiscard]] auto add(i32 fd, IoEvent events) -> Result<Unit> { return ctl(EPOLL_CTL_ADD, fd, events); }

    [[nodiscard]] auto modify(i32 fd, IoEvent events) -> Result<Unit> { return ctl(EPOLL_CTL_MOD, fd, events); }

    [[nodiscard]] auto remove(i32 fd) -> Result<Unit>
    {
        if (fd < 0)
            return Ok();
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0 && errno != ENOENT && errno != EBADF)
            return Err(SystemError::from_errno());
        clear_slot(fd);
        return Ok();
    }

    [[nodiscard]] auto poll(Duration timeout) -> Result<u32>
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
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
            if (fd < 0 || static_cast<usize>(fd) >= slots_.size())
                continue;
            auto& e = slots_[static_cast<usize>(fd)];
            const bool err = (ep & (EPOLLERR | EPOLLHUP)) != 0;
            if (!e.read && !e.write) {
                if (err && e.registered)
                    (void)remove(fd); // HUP/ERR 是持续状态，移除避免空转
                continue;
            }
            // 错误/挂断唤醒两个方向，否则按 EPOLLIN/EPOLLOUT 分派。
            if (e.read && ((ep & EPOLLIN) || err)) {
                if (fire(*e.read))
                    ++completed;
                e.read.reset();
            }
            if (e.write && ((ep & EPOLLOUT) || err)) {
                if (fire(*e.write))
                    ++completed;
                e.write.reset();
            }
            if (err && !e.read && !e.write && e.registered)
                (void)remove(fd);
        }
        return Ok(completed);
    }

    [[nodiscard]] auto register_read(i32 fd,
                                     std::coroutine_handle<> h,
                                     u8* /*buf*/,
                                     usize /*len*/,
                                     Result<usize>* result_ptr,
                                     WaitState* state = nullptr,
                                     WaitState completion = WaitState::Ready) -> Result<Unit>
    {
        return register_io(false, fd, h, result_ptr, state, completion);
    }

    [[nodiscard]] auto register_write(i32 fd,
                                      std::coroutine_handle<> h,
                                      const u8* /*buf*/,
                                      usize /*len*/,
                                      Result<usize>* result_ptr,
                                      WaitState* state = nullptr,
                                      WaitState completion = WaitState::Ready) -> Result<Unit>
    {
        return register_io(true, fd, h, result_ptr, state, completion);
    }

    auto deregister(i32 fd) -> Unit
    {
        (void)remove(fd);
        return unit;
    }

    // 仅清指定方向的等待者；持久注册保留，下一次等待无需 epoll_ctl。
    auto deregister(i32 fd, IoEvent dir) -> Unit
    {
        if (fd < 0 || static_cast<usize>(fd) >= slots_.size())
            return unit;
        auto& e = slots_[static_cast<usize>(fd)];
        if (has_flag(dir, IoEvent::Read))
            e.read.reset();
        if (has_flag(dir, IoEvent::Write))
            e.write.reset();
        return unit;
    }

    [[nodiscard]] auto drain_ready() -> std::vector<std::coroutine_handle<>> { return std::exchange(ready_, { }); }

    auto drain_ready_into(std::vector<std::coroutine_handle<>>& out) -> void
    {
        out.insert(out.end(), ready_.begin(), ready_.end());
        ready_.clear();
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return epoll_fd_ >= 0; }

    [[nodiscard]] auto close() -> Result<Unit>
    {
        std::optional<SystemError> first_error;
        if (epoll_fd_ >= 0) {
            if (::close(epoll_fd_) < 0)
                first_error = SystemError::from_errno();
            epoll_fd_ = -1;
        }
        slots_.clear();
        ready_.clear();
        if (first_error.has_value())
            return Err(std::move(*first_error));
        return Ok();
    }

private:
    struct PendingEntry {
        std::coroutine_handle<> handle { };
        Result<usize>* result = nullptr;
        WaitState* state = nullptr;
        WaitState completion = WaitState::Ready;
    };

    // 每个 fd 可同时挂起一个读、一个写等待者，支持全双工。
    struct FdEntry {
        std::optional<PendingEntry> read;
        std::optional<PendingEntry> write;
        bool registered = false;
    };

    explicit EpollReactor(i32 epoll_fd) : epoll_fd_(epoll_fd), events_(kMaxEventsPerPoll) { }

    static auto error(std::errc code, const char* msg) -> SystemError
    {
        return { .code = std::make_error_code(code), .message = msg, .location = std::source_location::current() };
    }

    [[nodiscard]] auto ctl(i32 op, i32 fd, IoEvent events) -> Result<Unit>
    {
        struct epoll_event ev { };
        ev.events = detail::to_epoll_events(events);
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, op, fd, &ev) < 0)
            return Err(SystemError::from_errno());
        return Ok();
    }

    [[nodiscard]] auto register_persistent(i32 fd) -> Result<Unit>
    {
        struct epoll_event ev { };
        ev.events = detail::to_epoll_events(IoEvent::Read | IoEvent::Write | IoEvent::EdgeTrig);
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0)
            return Ok();
        const int err = errno;
        if (err != EEXIST) {
            return Err(SystemError { .code = std::error_code(err, std::system_category()),
                                     .message = std::system_category().message(err),
                                     .location = std::source_location::current() });
        }
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0)
            return Ok();
        return Err(SystemError::from_errno());
    }

    [[nodiscard]] auto register_io(bool write_dir, i32 fd, std::coroutine_handle<> h, Result<usize>* result_ptr,
                                   WaitState* state, WaitState completion) -> Result<Unit>
    {
        if (fd < 0)
            return Err(error(std::errc::bad_file_descriptor, write_dir ? "register_write: negative fd"
                                                                       : "register_read: negative fd"));
        auto& entry = ensure_slot(fd);
        auto& slot = write_dir ? entry.write : entry.read;
        if (slot)
            return Err(error(std::errc::device_or_resource_busy,
                write_dir ? "register_write: write already pending on fd" : "register_read: read already pending on fd"));
        if (!entry.registered) {
            if (auto reg = register_persistent(fd); !reg)
                return Err(std::move(reg).error());
            entry.registered = true;
        }
        slot = PendingEntry { .handle = h, .result = result_ptr, .state = state, .completion = completion };
        return Ok();
    }

    // 触发一个方向：处理与定时器的 state 竞争、写结果、入队句柄。
    auto fire(const PendingEntry& entry) -> bool
    {
        if (entry.state) {
            if (*entry.state != WaitState::Pending)
                return false;
            *entry.state = entry.completion;
        }
        if (entry.result)
            *entry.result = Ok(usize { 0 });
        if (entry.handle) {
            ready_.push_back(entry.handle);
            return true;
        }
        return false;
    }

    [[nodiscard]] auto ensure_slot(i32 fd) -> FdEntry&
    {
        const auto idx = static_cast<usize>(fd);
        if (idx >= slots_.size())
            slots_.resize(idx + 1);
        return slots_[idx];
    }

    auto clear_slot(i32 fd) -> void
    {
        if (fd < 0)
            return;
        const auto idx = static_cast<usize>(fd);
        if (idx < slots_.size()) {
            slots_[idx].read.reset();
            slots_[idx].write.reset();
            slots_[idx].registered = false;
        }
    }

    i32 epoll_fd_ = -1;
    std::vector<epoll_event> events_;
    std::vector<FdEntry> slots_;
    std::vector<std::coroutine_handle<>> ready_;
};
static_assert(IoReactorLike<EpollReactor>);

} // namespace rain::async
