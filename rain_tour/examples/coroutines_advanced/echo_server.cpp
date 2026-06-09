#include <cerrno>
#include <coroutine>
#include <cstdint>
#include <csignal>
#include <cstring>
#include <expected>
#include <format>
#include <iostream>
#include <string>
#include <unordered_map>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr std::uint16_t kPort = 9200;
constexpr int kBacklog = 128;
constexpr int kMaxEvents = 64;
constexpr int kBufSize = 4096;

// ── Result<T>：用 std::expected 表达「值或错误」，不靠异常 ──────────
// 这正是 Rain core/result.hpp 的 Result<T, E> 思路：错误是值，显式传播。
template <typename T>
using Result = std::expected<T, std::string>;

[[nodiscard]] auto Err(std::string msg) -> std::unexpected<std::string>
{
    return std::unexpected { std::move(msg) };
}

// ── 事件循环的全局状态（单线程，无需加锁）────────────────────────
int g_epfd = -1;
std::unordered_map<int, std::coroutine_handle<>> g_waiters; // fd -> 等待该 fd 的协程

// ── 纯函数：构造地址 / epoll 事件，输入定输出、无副作用 ────────────
[[nodiscard]] auto make_addr(std::uint16_t port) -> sockaddr_in
{
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    return addr;
}

[[nodiscard]] auto make_event(int fd, std::uint32_t events) -> epoll_event
{
    epoll_event ev {};
    ev.events = events | EPOLLONESHOT; // 触发一次后自动失效，下次 co_await 再武装
    ev.data.fd = fd;
    return ev;
}

void set_nonblocking(int fd)
{
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// ── fire-and-forget 协程：启动即运行，完成即自毁 ──────────────────
// final_suspend 返回 suspend_never，协程返回时帧自动销毁，
// 所以每个连接的协程不需要外部持有 handle —— 它自己管理生命周期。
struct DetachedTask {
    struct promise_type {
        [[nodiscard]] auto get_return_object() noexcept -> DetachedTask { return {}; }
        [[nodiscard]] auto initial_suspend() noexcept -> std::suspend_never { return {}; }
        [[nodiscard]] auto final_suspend() noexcept -> std::suspend_never { return {}; }
        void return_void() noexcept { }
        void unhandled_exception() { std::terminate(); }
    };
};

// ── I/O awaiter：把 fd 注册进 epoll 并挂起协程 ─────────────────────
struct IoAwaiter {
    int fd;
    std::uint32_t events; // EPOLLIN 或 EPOLLOUT

    [[nodiscard]] auto await_ready() const noexcept -> bool { return false; }

    void await_suspend(std::coroutine_handle<> h) const
    {
        g_waiters[fd] = h;
        const epoll_event ev = make_event(fd, events);
        // 首次 ADD，之后 MOD 重新武装；这里 MOD 失败回退 ADD
        if (epoll_ctl(g_epfd, EPOLL_CTL_MOD, fd, const_cast<epoll_event*>(&ev)) < 0) {
            epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, const_cast<epoll_event*>(&ev));
        }
    }

    void await_resume() const noexcept { }
};

[[nodiscard]] auto wait_readable(int fd) -> IoAwaiter { return IoAwaiter { fd, EPOLLIN }; }
[[nodiscard]] auto wait_writable(int fd) -> IoAwaiter { return IoAwaiter { fd, EPOLLOUT }; }

// ── 每个连接一个协程：读 → 写回 → 循环，直到对端关闭 ──────────────
// I/O 本质是有副作用的，所以这段保持命令式 —— 函数式不是假装 I/O 是纯的，
// 而是把纯逻辑（上面的 make_event 等）和副作用清晰分开。
auto handle_connection(int conn) -> DetachedTask
{
    set_nonblocking(conn);
    std::cout << std::format("  连接 fd={} 开始 echo\n", conn);

    char buf[kBufSize];
    bool open = true;
    while (open) {
        co_await wait_readable(conn);

        const ssize_t n = ::read(conn, buf, sizeof(buf));
        if (n < 0 && errno == EAGAIN) {
            continue; // 虚假唤醒，继续等可读
        }
        if (n <= 0) {
            break; // 对端关闭或出错
        }

        ssize_t off = 0;
        while (off < n) {
            const ssize_t w = ::write(conn, buf + off, static_cast<std::size_t>(n - off));
            if (w < 0) {
                if (errno == EAGAIN) {
                    co_await wait_writable(conn); // 写缓冲满，等可写
                    continue;
                }
                open = false;
                break;
            }
            off += w;
        }
    }

    epoll_ctl(g_epfd, EPOLL_CTL_DEL, conn, nullptr);
    g_waiters.erase(conn);
    ::close(conn);
    std::cout << std::format("  连接 fd={} 关闭\n", conn);
}

// ── accept 协程：等监听 fd 可读，把新连接交给 handle_connection ────
auto accept_loop(int listener) -> DetachedTask
{
    for (;;) {
        co_await wait_readable(listener);
        for (;;) {
            const int conn = ::accept(listener, nullptr, nullptr);
            if (conn < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; // 已全部取走
                }
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            handle_connection(conn); // fire-and-forget，挂起在首个 co_await
        }
    }
}

// ── 副作用工厂：创建 epoll / 监听 socket，失败返回 Err ────────────
[[nodiscard]] auto make_epoll() -> Result<int>
{
    const int fd = epoll_create1(0);
    if (fd < 0) {
        return Err(std::format("epoll_create1: {}", std::strerror(errno)));
    }
    return fd;
}

[[nodiscard]] auto make_listener(std::uint16_t port) -> Result<int>
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return Err(std::format("socket: {}", std::strerror(errno)));
    }

    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    const sockaddr_in addr = make_addr(port);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return Err(std::format("bind: {}", std::strerror(errno)));
    }
    if (::listen(fd, kBacklog) < 0) {
        ::close(fd);
        return Err(std::format("listen: {}", std::strerror(errno)));
    }
    set_nonblocking(fd);
    return fd;
}

// ── 事件循环：epoll_wait 等就绪，恢复对应协程 ────────────────────
void run()
{
    epoll_event evs[kMaxEvents];
    for (;;) {
        const int n = epoll_wait(g_epfd, evs, kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << std::format("epoll_wait: {}\n", std::strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            const int fd = evs[i].data.fd;
            const auto it = g_waiters.find(fd);
            if (it == g_waiters.end()) {
                continue;
            }
            const auto h = it->second;
            g_waiters.erase(it); // EPOLLONESHOT 已禁用该 fd，取出即清
            h.resume();          // 恢复协程，它会在下次 co_await 重新武装
        }
    }
}

} // namespace

/*
 * 单线程 epoll + C++20 协程 echo 服务器（函数式风格的错误处理）。
 *
 * 这是 rain_07 io_awaiter_sketch 的完整化：从「一个 fd 的 awaiter 草图」
 * 扩展成「epoll 驱动、多连接并发」的真实服务器。
 *
 * 核心结构（正是 Rain EventLoop 的缩影）：
 *   - DetachedTask  : fire-and-forget 协程，每个连接一个，完成即自毁；
 *   - IoAwaiter     : await_suspend 把 fd+handle 注册进 epoll，挂起协程；
 *   - run()         : epoll_wait 等就绪，恢复等待该 fd 的协程。
 *
 * 函数式要点：用 Result<T>（std::expected）表达可失败的初始化，错误是值、
 * 显式传播，不靠异常 —— 与 Rain 的 Result<T, E> / Ok / Err 完全同源。
 * 纯函数（make_addr / make_event）与副作用（syscall）分层清晰。
 *
 * 测试：
 *   make run-echo
 *   nc 127.0.0.1 9200      # 输入任意文字会被原样回显
 */
int main()
{
    std::signal(SIGPIPE, SIG_IGN);

    const auto ep = make_epoll();
    if (!ep) {
        std::cerr << std::format("启动失败: {}\n", ep.error());
        return 1;
    }
    g_epfd = *ep;

    const auto listener = make_listener(kPort);
    if (!listener) {
        std::cerr << std::format("启动失败: {}\n", listener.error());
        return 1;
    }

    accept_loop(*listener); // 启动 accept 协程，挂起在监听 fd 上
    std::cout << std::format("协程 echo 服务器监听 :{}（Ctrl-C 退出）\n", kPort);

    run();

    ::close(*listener);
    ::close(g_epfd);
    return 0;
}
