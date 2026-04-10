#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/concepts.hpp"
#include "async/io_reactor.hpp"
#include "async/task.hpp"
#include "net/accept_awaiter.hpp"
#include "net/address.hpp"
#include "net/listen_options.hpp"
#include "net/listener_socket_ops.hpp"
#include "net/socket_fd_ops.hpp"
#include "net/tcp_stream.hpp"

#include <utility>

namespace rain::net {

// ── 监听选项（ListenOptions）─────────────────────────────────────────

// ── 监听器（TcpListener）─────────────────────────────────────────────
// 异步 TCP 监听套接字。支持 SO_REUSEPORT 以实现每核心 accept。
//
// 与 Executor 配合用法：
//   executor.run([](EventLoop& loop, usize) -> Result<Unit> {
//       auto addr = SocketAddress::any(8080);
//       auto listener = TcpListener::bind(loop.reactor(), addr);
//       loop.spawn(accept_loop(std::move(*listener)));
//       return Ok();
//   });

class TcpListener {
public:
    TcpListener(const TcpListener&) = delete;
    auto operator=(const TcpListener&) -> TcpListener& = delete;

    TcpListener(TcpListener&& other) noexcept : fd_(std::exchange(other.fd_, -1)), reactor_(other.reactor_) { }

    auto operator=(TcpListener&& other) noexcept -> TcpListener&
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

    ~TcpListener()
    {
        if (auto close_result = close(); !close_result) {
            // 析构路径仅做 best-effort。
        }
    }

    // -- 工厂方法 ------------------------------------------------------

    [[nodiscard]] static auto bind(async::EpollReactor& reactor,
                                   const SocketAddress& addr,
                                   const ListenOptions& opts = { }) -> Result<TcpListener>
    {
        auto fd_result = detail::bind_listener_socket(addr, opts);
        if (!fd_result) {
            return Err(std::move(fd_result).error());
        }
        return Ok(TcpListener(*fd_result, reactor));
    }

    // -- 异步 accept ---------------------------------------------------

    [[nodiscard]] auto accept() -> async::Task<Result<std::pair<TcpStream, SocketAddress>>>
    {
        AcceptAwaiter awaiter(*reactor_, fd_);
        auto result = co_await awaiter;

        if (!result) {
            co_return Err(std::move(result).error());
        }

        auto [conn_fd, peer_addr] = std::move(*result);

        auto stream = TcpStream::from_fd(*reactor_, conn_fd);
        if (!stream) {
            ::close(conn_fd);
            co_return Err(std::move(stream).error());
        }

        co_return Ok(std::pair { std::move(*stream), std::move(peer_addr) });
    }

    // -- 访问器 --------------------------------------------------------

    [[nodiscard]] auto fd() const noexcept -> i32 { return fd_; }

    [[nodiscard]] auto local_address() const -> Result<SocketAddress> { return detail::local_address(fd_); }

    [[nodiscard]] auto valid() const noexcept -> bool { return fd_ >= 0; }

    [[nodiscard]] auto close() -> Result<Unit> { return detail::close_fd(reactor_, fd_); }

private:
    TcpListener(i32 fd, async::EpollReactor& reactor) : fd_(fd), reactor_(&reactor) { }

    i32 fd_ = -1;
    async::EpollReactor* reactor_ = nullptr;
};

} // namespace rain::net
