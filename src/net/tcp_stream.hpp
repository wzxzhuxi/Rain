#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/event_loop.hpp"
#include "async/io_reactor.hpp"
#include "async/task.hpp"
#include "net/address.hpp"
#include "net/io_awaiters.hpp"
#include "net/socket_fd_ops.hpp"
#include "net/stream_io_ops.hpp"

#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace rain::net {
// RAII 异步 TCP 连接：持有 fd，I/O 时由 reactor 按需注册。
class TcpStream {
public:
    TcpStream(const TcpStream&) = delete;
    auto operator=(const TcpStream&) -> TcpStream& = delete;

    TcpStream(TcpStream&& other) noexcept : fd_(std::exchange(other.fd_, -1)), reactor_(other.reactor_) { }

    auto operator=(TcpStream&& other) noexcept -> TcpStream&
    {
        if (this != &other) {
            (void)close();
            fd_ = std::exchange(other.fd_, -1);
            reactor_ = other.reactor_;
        }
        return *this;
    }

    ~TcpStream() { (void)close(); }

    [[nodiscard]] static auto connect(async::EpollReactor& reactor, const SocketAddress& addr)
        -> async::Task<Result<TcpStream>>
    {
        const i32 fd = ::socket(addr.family(), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            co_return Err(SystemError::from_errno());

        if (::connect(fd, addr.as_sockaddr(), addr.sockaddr_len()) == 0) {
            auto stream = TcpStream::from_fd(reactor, fd);
            if (!stream) {
                ::close(fd);
                co_return Err(std::move(stream).error());
            }
            co_return Ok(std::move(*stream));
        }
        if (errno != EINPROGRESS) {
            auto err = SystemError::from_errno();
            ::close(fd);
            co_return Err(std::move(err));
        }

        ConnectAwaiter awaiter(reactor, fd);
        auto connected = co_await awaiter;
        if (!connected) {
            (void)reactor.remove(fd);
            ::close(fd);
            co_return Err(std::move(connected).error());
        }
        co_return Ok(TcpStream(fd, reactor));
    }

    [[nodiscard]] static auto from_fd(async::EpollReactor& reactor, i32 fd) -> Result<TcpStream>
    {
        if (auto nonblocking = detail::ensure_nonblocking(fd); !nonblocking)
            return Err(std::move(nonblocking).error());
        return Ok(TcpStream(fd, reactor));
    }

    // 以下方法直接转发到 detail:: 协程：省去包装帧，await 期间不依赖本对象存活。
    [[nodiscard]] auto read(u8* buf, usize len) -> async::Task<Result<usize>>
    {
        return detail::read_some(*reactor_, fd_, buf, len);
    }

    [[nodiscard]] auto read_with_timeout(u8* buf, usize len, async::Duration timeout) -> async::Task<Result<usize>>
    {
        return detail::read_some_with_timeout(*reactor_, fd_, buf, len, timeout);
    }

    [[nodiscard]] auto write(const u8* buf, usize len) -> async::Task<Result<usize>>
    {
        return detail::write_some(*reactor_, fd_, buf, len);
    }

    [[nodiscard]] auto write_with_timeout(const u8* buf, usize len, async::Duration timeout)
        -> async::Task<Result<usize>>
    {
        return detail::write_some_with_timeout(*reactor_, fd_, buf, len, timeout);
    }

    [[nodiscard]] auto write_all(const u8* buf, usize len) -> async::Task<Result<usize>>
    {
        return detail::write_all(*reactor_, fd_, buf, len);
    }

    [[nodiscard]] auto write_all_with_timeout(const u8* buf, usize len, async::Duration timeout)
        -> async::Task<Result<usize>>
    {
        return detail::write_all_with_timeout(*reactor_, fd_, buf, len, timeout);
    }

    [[nodiscard]] auto send_file(i32 in_fd, off_t& offset, usize count) -> async::Task<Result<usize>>
    {
        return detail::send_file_some(*reactor_, fd_, in_fd, offset, count);
    }

    [[nodiscard]] auto send_file_with_timeout(i32 in_fd, off_t& offset, usize count, async::Duration timeout)
        -> async::Task<Result<usize>>
    {
        return detail::send_file_some_with_timeout(*reactor_, fd_, in_fd, offset, count, timeout);
    }

    [[nodiscard]] auto send_file_all_with_timeout(i32 in_fd, off_t offset, usize count, async::Duration timeout)
        -> async::Task<Result<usize>>
    {
        return detail::send_file_all_with_timeout(*reactor_, fd_, in_fd, offset, count, timeout);
    }

    [[nodiscard]] auto set_nodelay(bool enable) -> Result<Unit> { return detail::set_nodelay(fd_, enable); }
    [[nodiscard]] auto set_keepalive(bool enable) -> Result<Unit> { return detail::set_keepalive(fd_, enable); }

    [[nodiscard]] auto fd() const noexcept -> i32 { return fd_; }
    [[nodiscard]] auto peer_address() const -> Result<SocketAddress> { return detail::peer_address(fd_); }
    [[nodiscard]] auto local_address() const -> Result<SocketAddress> { return detail::local_address(fd_); }

    [[nodiscard]] auto shutdown(i32 how = SHUT_RDWR) -> Result<Unit> { return detail::shutdown_socket(fd_, how); }
    [[nodiscard]] auto valid() const noexcept -> bool { return fd_ >= 0; }
    [[nodiscard]] auto close() -> Result<Unit> { return detail::close_fd(reactor_, fd_); }

private:
    TcpStream(i32 fd, async::EpollReactor& reactor) : fd_(fd), reactor_(&reactor) { }

    i32 fd_ = -1;
    async::EpollReactor* reactor_ = nullptr;
};
} // namespace rain::net
