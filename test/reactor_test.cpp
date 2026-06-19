// Reactor 测试：socketpair 提供真实但本地确定的就绪，无端口、无 sleep。
// 覆盖 M7（同一 fd 并发读+写）与基础就绪/注销路径。

#include "async/io_reactor.hpp"
#include "testkit.hpp"

#include <chrono>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace rain;
using namespace rain::async;
using namespace std::chrono;

static auto err(const char* m) -> SystemError
{
    return SystemError { .code = std::make_error_code(std::errc::operation_canceled),
                         .message = m,
                         .location = std::source_location::current() };
}

auto main() -> int
{
    rain::test::Context ctx("reactor");

    auto rx = EpollReactor::create();
    TK_CHECK(ctx, rx.has_value());
    if (!rx)
        return ctx.summary();

    int sv[2];
    TK_CHECK(ctx, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    (void)::fcntl(sv[0], F_SETFL, O_NONBLOCK);

    // M7：同一 fd 上读和写可同时挂起（旧实现会以 device_or_resource_busy 拒绝第二个）。
    Result<usize> rr = Err(err("read pending"));
    Result<usize> wr = Err(err("write pending"));
    TK_CHECK(ctx, rx->register_read(sv[0], {}, nullptr, 0, &rr).has_value());
    TK_CHECK(ctx, rx->register_write(sv[0], {}, nullptr, 0, &wr).has_value());

    // 新 socketpair 可写不可读：写方向应触发，读方向保持挂起（独立分派）。
    (void)rx->poll(milliseconds(50));
    TK_CHECK(ctx, wr.has_value());
    TK_CHECK(ctx, !rr.has_value());

    // 写入对端使 sv[0] 可读；存活的读方向应在 ONESHOT 重新武装后触发。
    const char c = 'x';
    TK_CHECK(ctx, ::write(sv[1], &c, 1) == 1);
    (void)rx->poll(milliseconds(50));
    TK_CHECK(ctx, rr.has_value());

    // 同方向重复注册应被拒绝（一个方向至多一个等待者）。
    {
        Result<usize> r2 = Err(err("r2"));
        TK_CHECK(ctx, rx->register_read(sv[0], {}, nullptr, 0, &r2).has_value());
        Result<usize> r3 = Err(err("r3"));
        TK_CHECK(ctx, !rx->register_read(sv[0], {}, nullptr, 0, &r3).has_value());
        rx->deregister(sv[0]);
    }

    ::close(sv[0]);
    ::close(sv[1]);
    return ctx.summary();
}
