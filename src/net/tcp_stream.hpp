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

// ── 连接流（TcpStream）───────────────────────────────────────────────
// RAII 异步 TCP 连接。持有 socket fd，
// 在协程真正等待 I/O 时再由 reactor 按需注册（ensure add-or-mod）。

class TcpStream {
public:
    TcpStream(const TcpStream&) = delete;
    auto operator=(const TcpStream&) -> TcpStream& = delete;

    TcpStream(TcpStream&& other) noexcept : fd_(std::exchange(other.fd_, -1)), reactor_(other.reactor_) { }

    auto operator=(TcpStream&& other) noexcept -> TcpStream&
    {
        if (this != &other) {
            if (auto close_result = close(); !close_result) {
                // 移动赋值清理路径仅做 best-effort。
            }
            fd_ = std::exchange(other.fd_, -1);
            reactor_ = other.reactor_;
        }
        return *this;
    }

    ~TcpStream()
    {
        if (auto close_result = close(); !close_result) {
            // 析构路径仅做 best-effort。
        }
    }

    // -- 工厂方法：主动连接 --------------------------------------------

    [[nodiscard]] static auto connect(async::EpollReactor& reactor, const SocketAddress& addr)
        -> async::Task<Result<TcpStream>>
    {
        const i32 fd = ::socket(addr.family(), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            co_return Err(SystemError::from_errno());
        }

        const i32 ret = ::connect(fd, addr.as_sockaddr(), addr.sockaddr_len());
        if (ret == 0) {
            // 立即建立连接（如 localhost）
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

        // 等待 connect 完成
        ConnectAwaiter awaiter(reactor, fd);
        auto connect_result = co_await awaiter;

        if (!connect_result) {
            if (auto remove_result = reactor.remove(fd); !remove_result) {
                // 连接失败后的清理路径仅做 best-effort。
            }
            ::close(fd);
            co_return Err(std::move(connect_result).error());
        }

        co_return Ok(TcpStream(fd, reactor));
    }

    // -- 工厂方法：从已 accept 的 fd 构造（供 TcpListener 使用）--------

    [[nodiscard]] static auto from_fd(async::EpollReactor& reactor, i32 fd) -> Result<TcpStream>
    {
        auto nonblocking_result = detail::ensure_nonblocking(fd);
        if (!nonblocking_result) {
            return Err(std::move(nonblocking_result).error());
        }
        return Ok(TcpStream(fd, reactor));
    }

    // -- 异步 I/O ------------------------------------------------------

    [[nodiscard]] auto read(u8* buf, usize len) -> async::Task<Result<usize>>
    {
        co_return co_await detail::read_some(*reactor_, fd_, buf, len);
    }

    [[nodiscard]] auto read_with_timeout(u8* buf, usize len, async::Duration timeout) -> async::Task<Result<usize>>
    {
        co_return co_await detail::read_some_with_timeout(*reactor_, fd_, buf, len, timeout);
    }

    [[nodiscard]] auto write(const u8* buf, usize len) -> async::Task<Result<usize>>
    {
        co_return co_await detail::write_some(*reactor_, fd_, buf, len);
    }

    [[nodiscard]] auto write_with_timeout(const u8* buf, usize len, async::Duration timeout)
        -> async::Task<Result<usize>>
    {
        co_return co_await detail::write_some_with_timeout(*reactor_, fd_, buf, len, timeout);
    }

    // 写入全部字节，处理部分写场景。
    [[nodiscard]] auto write_all(const u8* buf, usize len) -> async::Task<Result<usize>>
    {
        co_return co_await detail::write_all(*reactor_, fd_, buf, len);
    }

    // 在超时预算内写入全部字节，处理部分写场景。
    [[nodiscard]] auto write_all_with_timeout(const u8* buf, usize len, async::Duration timeout)
        -> async::Task<Result<usize>>
    {
        co_return co_await detail::write_all_with_timeout(*reactor_, fd_, buf, len, timeout);
    }

    [[nodiscard]] auto send_file(i32 in_fd, off_t& offset, usize count) -> async::Task<Result<usize>>
    {
        co_return co_await detail::send_file_some(*reactor_, fd_, in_fd, offset, count);
    }

    [[nodiscard]] auto send_file_with_timeout(i32 in_fd, off_t& offset, usize count, async::Duration timeout)
        -> async::Task<Result<usize>>
    {
        co_return co_await detail::send_file_some_with_timeout(*reactor_, fd_, in_fd, offset, count, timeout);
    }

    [[nodiscard]] auto send_file_all_with_timeout(i32 in_fd, off_t offset, usize count, async::Duration timeout)
        -> async::Task<Result<usize>>
    {
        co_return co_await detail::send_file_all_with_timeout(*reactor_, fd_, in_fd, offset, count, timeout);
    }

    // -- 套接字选项 ----------------------------------------------------

    [[nodiscard]] auto set_nodelay(bool enable) -> Result<Unit> { return detail::set_nodelay(fd_, enable); }

    [[nodiscard]] auto set_keepalive(bool enable) -> Result<Unit> { return detail::set_keepalive(fd_, enable); }

    // -- 访问器 --------------------------------------------------------

    [[nodiscard]] auto fd() const noexcept -> i32 { return fd_; }

    [[nodiscard]] auto peer_address() const -> Result<SocketAddress> { return detail::peer_address(fd_); }

    [[nodiscard]] auto local_address() const -> Result<SocketAddress> { return detail::local_address(fd_); }

    // -- 显式关闭 ------------------------------------------------------

    [[nodiscard]] auto shutdown(i32 how = SHUT_RDWR) -> Result<Unit> { return detail::shutdown_socket(fd_, how); }

    [[nodiscard]] auto valid() const noexcept -> bool { return fd_ >= 0; }

    [[nodiscard]] auto close() -> Result<Unit> { return detail::close_fd(reactor_, fd_); }

private:
    TcpStream(i32 fd, async::EpollReactor& reactor) : fd_(fd), reactor_(&reactor) { }

    i32 fd_ = -1;
    async::EpollReactor* reactor_ = nullptr;
};

} // namespace rain::net
