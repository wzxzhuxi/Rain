#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/event_loop.hpp"
#include "async/io_reactor.hpp"
#include "net/address.hpp"

#include <coroutine>
#include <functional>
#include <optional>
#include <stop_token>
#include <sys/socket.h>
#include <utility>

namespace rain::net {

class AcceptAwaiter {
public:
    AcceptAwaiter(async::EpollReactor& reactor, i32 listen_fd) : reactor_(reactor), listen_fd_(listen_fd) { }

    // 注入取消令牌：优雅停机时取消挂起的 accept，使 accept loop 退出（spawn_service 注入此令牌）。
    auto set_cancellation(std::stop_token token) -> void { token_ = std::move(token); }

    [[nodiscard]] auto await_ready() noexcept -> bool
    {
        struct sockaddr_storage peer { };
        socklen_t peer_len = sizeof(peer);
        const i32 fd
            = ::accept4(listen_fd_, reinterpret_cast<struct sockaddr*>(&peer), &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) {
            eager_fd_ = fd;
            eager_peer_ = peer;
            eager_peer_len_ = peer_len;
            return true;
        }
        return false;
    }

    auto await_suspend(std::coroutine_handle<> h) -> bool
    {
        handle_ = h;
        auto reg = reactor_.register_read(listen_fd_, h, nullptr, 0, &readiness_, &state_, async::WaitState::Ready);
        if (!reg) {
            readiness_ = Err(std::move(reg).error());
            return false;
        }
        if (token_.stop_possible())
            cancel_cb_.emplace(token_, [this] {
                if (state_ != async::WaitState::Pending)
                    return;
                state_ = async::WaitState::Cancelled;
                (void)reactor_.deregister(listen_fd_, async::IoEvent::Read);
                if (async::g_event_loop && handle_)
                    async::g_event_loop->notify(handle_);
            });
        return true;
    }

    [[nodiscard]] auto await_resume() -> Result<std::pair<i32, SocketAddress>>
    {
        cancel_cb_.reset();
        if (eager_fd_ >= 0) {
            auto peer_addr = SocketAddress::from_raw(eager_peer_, eager_peer_len_);
            return Ok(std::pair { eager_fd_, std::move(peer_addr) });
        }

        if (state_ == async::WaitState::Cancelled) {
            return Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                     .message = "accept cancelled (graceful shutdown)",
                                     .location = std::source_location::current() });
        }

        if (!readiness_) {
            return Err(std::move(readiness_).error());
        }

        struct sockaddr_storage peer_storage { };
        socklen_t peer_len = sizeof(peer_storage);

        const i32 conn_fd = ::accept4(
            listen_fd_, reinterpret_cast<struct sockaddr*>(&peer_storage), &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (conn_fd < 0) {
            return Err(SystemError::from_errno());
        }

        auto peer_addr = SocketAddress::from_raw(peer_storage, peer_len);
        return Ok(std::pair { conn_fd, std::move(peer_addr) });
    }

private:
    async::EpollReactor& reactor_;
    i32 listen_fd_;
    i32 eager_fd_ = -1;
    struct sockaddr_storage eager_peer_ { };
    socklen_t eager_peer_len_ = 0;
    std::coroutine_handle<> handle_ {};
    async::WaitState state_ = async::WaitState::Pending;
    std::stop_token token_ {};
    std::optional<std::stop_callback<std::function<void()>>> cancel_cb_ {};
    Result<usize> readiness_ = Err(SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                                                 .message = "accept not completed",
                                                 .location = std::source_location::current() });
};

} // namespace rain::net
