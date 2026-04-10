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
    if (flags < 0) {
        return Err(SystemError::from_errno());
    }
    if ((flags & O_NONBLOCK) == 0) {
        if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            return Err(SystemError::from_errno());
        }
    }
    return Ok();
}

[[nodiscard]] inline auto set_nodelay(i32 fd, bool enable) -> Result<Unit>
{
    const i32 val = enable ? 1 : 0;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) < 0) {
        return Err(SystemError::from_errno());
    }
    return Ok();
}

[[nodiscard]] inline auto set_keepalive(i32 fd, bool enable) -> Result<Unit>
{
    const i32 val = enable ? 1 : 0;
    if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) < 0) {
        return Err(SystemError::from_errno());
    }
    return Ok();
}

[[nodiscard]] inline auto peer_address(i32 fd) -> Result<SocketAddress>
{
    struct sockaddr_storage storage { };
    socklen_t len = sizeof(storage);
    if (::getpeername(fd, reinterpret_cast<struct sockaddr*>(&storage), &len) < 0) {
        return Err(SystemError::from_errno());
    }
    return Ok(SocketAddress::from_raw(storage, len));
}

[[nodiscard]] inline auto local_address(i32 fd) -> Result<SocketAddress>
{
    struct sockaddr_storage storage { };
    socklen_t len = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&storage), &len) < 0) {
        return Err(SystemError::from_errno());
    }
    return Ok(SocketAddress::from_raw(storage, len));
}

[[nodiscard]] inline auto shutdown_socket(i32 fd, i32 how) -> Result<Unit>
{
    if (::shutdown(fd, how) < 0) {
        return Err(SystemError::from_errno());
    }
    return Ok();
}

[[nodiscard]] inline auto close_fd(async::EpollReactor* reactor, i32& fd) -> Result<Unit>
{
    if (fd < 0) {
        return Ok();
    }

    std::optional<SystemError> first_error;
    if (reactor) {
        if (auto remove_result = reactor->remove(fd); !remove_result) {
            first_error = std::move(remove_result).error();
        }
    }
    if (::close(fd) < 0 && !first_error.has_value()) {
        first_error = SystemError::from_errno();
    }
    fd = -1;

    if (first_error.has_value()) {
        return Err(std::move(*first_error));
    }
    return Ok();
}

} // namespace rain::net::detail
