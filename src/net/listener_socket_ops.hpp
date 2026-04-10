#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "net/address.hpp"
#include "net/listen_options.hpp"

#include <sys/socket.h>
#include <unistd.h>

namespace rain::net::detail {

[[nodiscard]] inline auto bind_listener_socket(const SocketAddress& addr, const ListenOptions& opts) -> Result<i32>
{
    const i32 fd = ::socket(addr.family(), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return Err(SystemError::from_errno());
    }

    if (opts.reuse_addr) {
        const i32 val = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
            auto err = SystemError::from_errno();
            ::close(fd);
            return Err(std::move(err));
        }
    }

    if (opts.reuse_port) {
        const i32 val = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val)) < 0) {
            auto err = SystemError::from_errno();
            ::close(fd);
            return Err(std::move(err));
        }
    }

    if (::bind(fd, addr.as_sockaddr(), addr.sockaddr_len()) < 0) {
        auto err = SystemError::from_errno();
        ::close(fd);
        return Err(std::move(err));
    }

    if (::listen(fd, static_cast<i32>(opts.backlog)) < 0) {
        auto err = SystemError::from_errno();
        ::close(fd);
        return Err(std::move(err));
    }

    return Ok(fd);
}

} // namespace rain::net::detail
