#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/io_reactor.hpp"
#include "net/address.hpp"

#include <fcntl.h>
#include <netinet/tcp.h>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>

namespace rain::net::detail {
[[nodiscard]] inline auto ensure_nonblocking(i32 fd) -> Result<Unit>
{
    const i32 flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return Err(SystemError::from_errno());
    if ((flags & O_NONBLOCK) == 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return Err(SystemError::from_errno());
    return Ok();
}

[[nodiscard]] inline auto set_int_option(i32 fd, i32 level, i32 name, bool enable) -> Result<Unit>
{
    const i32 val = enable ? 1 : 0;
    if (::setsockopt(fd, level, name, &val, sizeof(val)) < 0)
        return Err(SystemError::from_errno());
    return Ok();
}

[[nodiscard]] inline auto set_nodelay(i32 fd, bool enable) -> Result<Unit>
{
    return set_int_option(fd, IPPROTO_TCP, TCP_NODELAY, enable);
}

[[nodiscard]] inline auto set_keepalive(i32 fd, bool enable) -> Result<Unit>
{
    return set_int_option(fd, SOL_SOCKET, SO_KEEPALIVE, enable);
}

[[nodiscard]] inline auto socket_address(i32 fd, bool peer) -> Result<SocketAddress>
{
    struct sockaddr_storage storage { };
    socklen_t len = sizeof(storage);
    const i32 rc = peer ? ::getpeername(fd, reinterpret_cast<struct sockaddr*>(&storage), &len)
                        : ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&storage), &len);
    if (rc < 0)
        return Err(SystemError::from_errno());
    return Ok(SocketAddress::from_raw(storage, len));
}

[[nodiscard]] inline auto peer_address(i32 fd) -> Result<SocketAddress> { return socket_address(fd, true); }
[[nodiscard]] inline auto local_address(i32 fd) -> Result<SocketAddress> { return socket_address(fd, false); }

[[nodiscard]] inline auto shutdown_socket(i32 fd, i32 how) -> Result<Unit>
{
    if (::shutdown(fd, how) < 0)
        return Err(SystemError::from_errno());
    return Ok();
}

[[nodiscard]] inline auto close_fd(async::EpollReactor* reactor, i32& fd) -> Result<Unit>
{
    if (fd < 0)
        return Ok();
    std::optional<SystemError> first_error;
    if (reactor) {
        if (auto removed = reactor->remove(fd); !removed)
            first_error = std::move(removed).error();
    }
    if (::close(fd) < 0 && !first_error.has_value())
        first_error = SystemError::from_errno();
    fd = -1;

    if (first_error.has_value())
        return Err(std::move(*first_error));
    return Ok();
}
} // namespace rain::net::detail
