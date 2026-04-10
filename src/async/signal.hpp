#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <coroutine>
#include <initializer_list>
#include <optional>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <utility>

namespace rain::async {

// ── SignalInfo——解析后的信号数据 ─────────────────────────────────────

struct SignalInfo {
    i32 signo; // 信号编号（SIGINT、SIGTERM 等）
    i32 pid;   // 发送方 PID
    i32 uid;   // 发送方 UID

    [[nodiscard]] static auto from_raw(const signalfd_siginfo& raw) -> SignalInfo
    {
        return {
            .signo = static_cast<i32>(raw.ssi_signo),
            .pid = static_cast<i32>(raw.ssi_pid),
            .uid = static_cast<i32>(raw.ssi_uid),
        };
    }
};

// ── SignalSet——RAII 封装 signalfd + sigprocmask ────────────────────
// 构造：通过 sigprocmask 屏蔽信号并创建 signalfd。
// 析构：关闭 signalfd，并恢复原始信号掩码。

class SignalSet {
public:
    SignalSet(const SignalSet&) = delete;
    auto operator=(const SignalSet&) -> SignalSet& = delete;

    SignalSet(SignalSet&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)), mask_(other.mask_), old_mask_(other.old_mask_)
    { }

    auto operator=(SignalSet&& other) noexcept -> SignalSet&
    {
        if (this != &other) {
            if (auto close_result = close(); !close_result) {
                // 移动赋值清理路径仅做 best-effort。
            }
            fd_ = std::exchange(other.fd_, -1);
            mask_ = other.mask_;
            old_mask_ = other.old_mask_;
        }
        return *this;
    }

    ~SignalSet()
    {
        if (auto close_result = close(); !close_result) {
            // 析构路径仅做 best-effort。
        }
    }

    [[nodiscard]] static auto create(std::initializer_list<i32> signals) -> Result<SignalSet>
    {
        sigset_t mask { };
        sigemptyset(&mask);
        for (const auto sig : signals) {
            sigaddset(&mask, sig);
        }

        sigset_t old_mask { };
        if (::sigprocmask(SIG_BLOCK, &mask, &old_mask) < 0) {
            return Err(SystemError::from_errno());
        }

        const i32 fd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (fd < 0) {
            ::sigprocmask(SIG_SETMASK, &old_mask, nullptr);
            return Err(SystemError::from_errno());
        }

        return Ok(SignalSet(fd, mask, old_mask));
    }

    [[nodiscard]] auto fd() const noexcept -> i32 { return fd_; }

    [[nodiscard]] auto read_signal() const -> Result<SignalInfo>
    {
        signalfd_siginfo info { };
        const auto n = ::read(fd_, &info, sizeof(info));
        if (n != static_cast<isize>(sizeof(info))) {
            if (n < 0) {
                return Err(SystemError::from_errno());
            }
            return Err(SystemError { .code = std::make_error_code(std::errc::message_size),
                                     .message = "signalfd short read",
                                     .location = std::source_location::current() });
        }
        return Ok(SignalInfo::from_raw(info));
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return fd_ >= 0; }

    [[nodiscard]] auto close() -> Result<Unit>
    {
        if (fd_ < 0) {
            return Ok();
        }

        std::optional<SystemError> first_error;
        if (::close(fd_) < 0) {
            first_error = SystemError::from_errno();
        }
        fd_ = -1;

        if (::sigprocmask(SIG_SETMASK, &old_mask_, nullptr) < 0 && !first_error.has_value()) {
            first_error = SystemError::from_errno();
        }

        if (first_error.has_value()) {
            return Err(std::move(*first_error));
        }
        return Ok();
    }

private:
    SignalSet(i32 fd, sigset_t mask, sigset_t old_mask) : fd_(fd), mask_(mask), old_mask_(old_mask) { }

    i32 fd_ = -1;
    sigset_t mask_ { };
    sigset_t old_mask_ { };
};

// ── SignalAwaiter——通过 reactor co_await 信号 ───────────────────────
// 使用仅就绪路径：reactor 只通知 signalfd 可读，
// await_resume() 再调用 read_signal() 获取实际 SignalInfo。
//
// 这里仅前向声明 EpollReactor；在实际构造 SignalAwaiter 时
// （event_loop.hpp 中）即可拿到完整类型。

class EpollReactor;

class SignalAwaiter {
public:
    SignalAwaiter(SignalSet& signals, EpollReactor& reactor) : signals_(signals), reactor_(&reactor) { }

    [[nodiscard]] auto await_ready() noexcept -> bool { return false; }

    // 在 EpollReactor 完整定义后再进行类外定义（见文件底部
    // 或 event_loop.hpp）。由于是 header-only，这里内联声明，
    // 但仍依赖 EpollReactor::register_read，因此在下方定义。
    inline auto await_suspend(std::coroutine_handle<> h) -> bool;

    [[nodiscard]] auto await_resume() -> Result<SignalInfo>
    {
        if (!readiness_) {
            return Err(std::move(readiness_).error());
        }
        return signals_.read_signal();
    }

private:
    SignalSet& signals_;
    EpollReactor* reactor_;
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "signal wait not completed",
                                                 .location = std::source_location::current() });
};

} // namespace rain::async
