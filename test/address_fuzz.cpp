// libFuzzer harness：地址解析/构造面。喂任意字节给 from_raw（含可超过 storage
// 大小的 len，检验夹紧）与字符串解析，确保任意畸形输入下无 UB/越界。
// 构建：cmake -DRAIN_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++  （需 libFuzzer）

#include "net/address.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/socket.h>

extern "C" auto LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) -> int
{
    using rain::net::SocketAddress;

    // from_raw + 访问器：任意字节构造 sockaddr_storage，len 可超过 sizeof（走夹紧路径）。
    {
        sockaddr_storage ss {};
        const size_t n = size < sizeof(ss) ? size : sizeof(ss);
        std::memcpy(&ss, data, n);
        const auto len = static_cast<socklen_t>(size % (sizeof(ss) + 16));
        auto addr = SocketAddress::from_raw(ss, len);
        (void)addr.family();
        (void)addr.port();
        (void)addr.ip_string();
        (void)addr.to_string();
        (void)addr.sockaddr_len();
    }

    // 字符串解析（inet_pton 路径）。
    {
        std::string s(reinterpret_cast<const char*>(data), size);
        (void)SocketAddress::from_ipv4(s, 80);
        (void)SocketAddress::from_ipv6(s, 80);
    }

    return 0;
}
