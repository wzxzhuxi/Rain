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
    if (fd < 0)
        return Err(SystemError::from_errno());

    auto fail = [fd]() {
        auto err = SystemError::from_errno();
        ::close(fd);
        return Err(std::move(err));
    };
    auto set_bool = [fd](i32 name) -> bool {
        const i32 val = 1;
        return ::setsockopt(fd, SOL_SOCKET, name, &val, sizeof(val)) == 0;
    };

    if (opts.reuse_addr && !set_bool(SO_REUSEADDR))
        return fail();
    if (opts.reuse_port && !set_bool(SO_REUSEPORT))
        return fail();
    if (::bind(fd, addr.as_sockaddr(), addr.sockaddr_len()) < 0)
        return fail();
    if (::listen(fd, static_cast<i32>(opts.backlog)) < 0)
        return fail();
    return Ok(fd);
}
} // namespace rain::net::detail
