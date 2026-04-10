#pragma once

#include "core/types.hpp"
#include "core/result.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>
#include <format>

namespace rain::net {

// ── 套接字地址（SocketAddress）───────────────────────────────────────
// sockaddr_storage 的类型安全包装，支持 IPv4 与 IPv6。
// 值类型：可拷贝、可移动、无堆分配。

class SocketAddress {
public:
    SocketAddress() { std::memset(&storage_, 0, sizeof(storage_)); }

    // -- 工厂方法 ------------------------------------------------------

    [[nodiscard]] static auto from_ipv4(StringView ip, u16 port)
        -> Result<SocketAddress>
    {
        SocketAddress addr;
        auto& sin = addr.as_v4();
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);

        const String ip_str{ip};
        if (::inet_pton(AF_INET, ip_str.c_str(), &sin.sin_addr) != 1) {
            return Err(SystemError{
                .code = std::make_error_code(std::errc::invalid_argument),
                .message = std::format("invalid IPv4 address: {}", ip),
                .location = std::source_location::current()
            });
        }
        return Ok(addr);
    }

    [[nodiscard]] static auto from_ipv6(StringView ip, u16 port)
        -> Result<SocketAddress>
    {
        SocketAddress addr;
        auto& sin6 = addr.as_v6();
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(port);

        const String ip_str{ip};
        if (::inet_pton(AF_INET6, ip_str.c_str(), &sin6.sin6_addr) != 1) {
            return Err(SystemError{
                .code = std::make_error_code(std::errc::invalid_argument),
                .message = std::format("invalid IPv6 address: {}", ip),
                .location = std::source_location::current()
            });
        }
        return Ok(addr);
    }

    // 绑定所有网卡（0.0.0.0:port）
    [[nodiscard]] static auto any(u16 port) -> SocketAddress {
        SocketAddress addr;
        auto& sin = addr.as_v4();
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
        return addr;
    }

    // 绑定本机回环（127.0.0.1:port）
    [[nodiscard]] static auto loopback(u16 port) -> SocketAddress {
        SocketAddress addr;
        auto& sin = addr.as_v4();
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return addr;
    }

    // 从原始 sockaddr 构造（供 accept4 使用）
    [[nodiscard]] static auto from_raw(const struct sockaddr_storage& raw,
                                        socklen_t len) -> SocketAddress {
        SocketAddress addr;
        std::memcpy(&addr.storage_, &raw, static_cast<usize>(len));
        return addr;
    }

    // -- 访问器 --------------------------------------------------------

    [[nodiscard]] auto family() const noexcept -> i32 {
        return storage_.ss_family;
    }

    [[nodiscard]] auto port() const noexcept -> u16 {
        if (family() == AF_INET) {
            return ntohs(as_v4().sin_port);
        }
        if (family() == AF_INET6) {
            return ntohs(as_v6().sin6_port);
        }
        return 0;
    }

    [[nodiscard]] auto ip_string() const -> String {
        char buf[INET6_ADDRSTRLEN]{};
        if (family() == AF_INET) {
            ::inet_ntop(AF_INET, &as_v4().sin_addr, buf, sizeof(buf));
        } else if (family() == AF_INET6) {
            ::inet_ntop(AF_INET6, &as_v6().sin6_addr, buf, sizeof(buf));
        }
        return String{buf};
    }

    [[nodiscard]] auto to_string() const -> String {
        if (family() == AF_INET6) {
            return std::format("[{}]:{}", ip_string(), port());
        }
        return std::format("{}:{}", ip_string(), port());
    }

    [[nodiscard]] auto as_sockaddr() const -> const struct sockaddr* {
        return reinterpret_cast<const struct sockaddr*>(&storage_);
    }

    [[nodiscard]] auto as_sockaddr_mut() -> struct sockaddr* {
        return reinterpret_cast<struct sockaddr*>(&storage_);
    }

    [[nodiscard]] auto sockaddr_len() const noexcept -> socklen_t {
        if (family() == AF_INET6) {
            return sizeof(struct sockaddr_in6);
        }
        return sizeof(struct sockaddr_in);
    }

    auto operator==(const SocketAddress& other) const -> bool {
        return family() == other.family()
            && port() == other.port()
            && ip_string() == other.ip_string();
    }

private:
    [[nodiscard]] auto as_v4() -> struct sockaddr_in& {
        return *reinterpret_cast<struct sockaddr_in*>(&storage_);
    }
    [[nodiscard]] auto as_v4() const -> const struct sockaddr_in& {
        return *reinterpret_cast<const struct sockaddr_in*>(&storage_);
    }
    [[nodiscard]] auto as_v6() -> struct sockaddr_in6& {
        return *reinterpret_cast<struct sockaddr_in6*>(&storage_);
    }
    [[nodiscard]] auto as_v6() const -> const struct sockaddr_in6& {
        return *reinterpret_cast<const struct sockaddr_in6*>(&storage_);
    }

    struct sockaddr_storage storage_{};
};

} // namespace rain::net
