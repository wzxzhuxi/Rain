#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <format>
#include <netinet/in.h>
#include <sys/socket.h>

namespace rain::net {
// sockaddr_storage 的类型安全值包装：可拷贝、无堆分配，支持 IPv4/IPv6。
class SocketAddress {
public:
    SocketAddress() { std::memset(&storage_, 0, sizeof(storage_)); }

    [[nodiscard]] static auto from_ipv4(StringView ip, u16 port) -> Result<SocketAddress>
    {
        return from_pton(AF_INET, ip, port);
    }

    [[nodiscard]] static auto from_ipv6(StringView ip, u16 port) -> Result<SocketAddress>
    {
        return from_pton(AF_INET6, ip, port);
    }

    [[nodiscard]] static auto any(u16 port) -> SocketAddress { return ipv4(htonl(INADDR_ANY), port); }
    [[nodiscard]] static auto loopback(u16 port) -> SocketAddress { return ipv4(htonl(INADDR_LOOPBACK), port); }

    // 供 accept4 使用；长度夹紧到 storage_ 大小，防止越界读写。
    [[nodiscard]] static auto from_raw(const struct sockaddr_storage& raw, socklen_t len) -> SocketAddress
    {
        SocketAddress addr;
        const usize n = std::min(static_cast<usize>(len), sizeof(addr.storage_));
        std::memcpy(&addr.storage_, &raw, n);
        return addr;
    }

    [[nodiscard]] auto family() const noexcept -> i32 { return storage_.ss_family; }

    [[nodiscard]] auto port() const noexcept -> u16
    {
        switch (family()) {
        case AF_INET:
            return ntohs(as_v4().sin_port);
        case AF_INET6:
            return ntohs(as_v6().sin6_port);
        default:
            return 0;
        }
    }

    [[nodiscard]] auto ip_string() const -> String
    {
        char buf[INET6_ADDRSTRLEN] { };
        if (family() == AF_INET)
            ::inet_ntop(AF_INET, &as_v4().sin_addr, buf, sizeof(buf));
        else if (family() == AF_INET6)
            ::inet_ntop(AF_INET6, &as_v6().sin6_addr, buf, sizeof(buf));
        return String { buf };
    }

    [[nodiscard]] auto to_string() const -> String
    {
        return family() == AF_INET6 ? std::format("[{}]:{}", ip_string(), port())
                                    : std::format("{}:{}", ip_string(), port());
    }

    [[nodiscard]] auto as_sockaddr() const -> const struct sockaddr*
    {
        return reinterpret_cast<const struct sockaddr*>(&storage_);
    }

    [[nodiscard]] auto as_sockaddr_mut() -> struct sockaddr*
    {
        return reinterpret_cast<struct sockaddr*>(&storage_);
    }

    [[nodiscard]] auto sockaddr_len() const noexcept -> socklen_t
    {
        return family() == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    }

    auto operator==(const SocketAddress& other) const -> bool
    {
        return family() == other.family() && port() == other.port() && ip_string() == other.ip_string();
    }

private:
    static auto ipv4(u32 addr, u16 port) -> SocketAddress
    {
        SocketAddress result;
        auto& sin = result.as_v4();
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        sin.sin_addr.s_addr = addr;
        return result;
    }

    static auto from_pton(i32 family, StringView ip, u16 port) -> Result<SocketAddress>
    {
        SocketAddress addr;
        addr.storage_.ss_family = static_cast<sa_family_t>(family);
        void* dst = nullptr;
        if (family == AF_INET) {
            addr.as_v4().sin_port = htons(port);
            dst = &addr.as_v4().sin_addr;
        } else {
            addr.as_v6().sin6_port = htons(port);
            dst = &addr.as_v6().sin6_addr;
        }
        const String text { ip };
        if (::inet_pton(family, text.c_str(), dst) != 1) {
            return Err(SystemError { .code = std::make_error_code(std::errc::invalid_argument),
                                     .message = std::format("invalid IP address: {}", ip),
                                     .location = std::source_location::current() });
        }
        return Ok(addr);
    }

    [[nodiscard]] auto as_v4() -> struct sockaddr_in&
    {
        return const_cast<struct sockaddr_in&>(static_cast<const SocketAddress&>(*this).as_v4());
    }
    [[nodiscard]] auto as_v4() const -> const struct sockaddr_in&
    {
        return *reinterpret_cast<const struct sockaddr_in*>(&storage_);
    }
    [[nodiscard]] auto as_v6() -> struct sockaddr_in6&
    {
        return const_cast<struct sockaddr_in6&>(static_cast<const SocketAddress&>(*this).as_v6());
    }
    [[nodiscard]] auto as_v6() const -> const struct sockaddr_in6&
    {
        return *reinterpret_cast<const struct sockaddr_in6*>(&storage_);
    }

    struct sockaddr_storage storage_ { };
};
} // namespace rain::net
