// fs/ 行为测试——填补"fs 零测试"的 blocker。fs 是 spawn_blocking 包 pread/pwrite/stat/unlink，
// 不碰真文件等于没测，故用真 /tmp + getpid 唯一名（无端口、无 sleep，与其他 syscall 测试同源）。
// 经 spawn_blocking 跨线程边界 → ASan/TSan 在此最该跑（最易出 lifetime/race）。

#include "core/result.hpp"
#include "core/types.hpp"

#include "fs/metadata.hpp"
#include "fs/ops.hpp"
#include "runtime/runtime.hpp"

#include "testkit.hpp"

#include <string>
#include <unistd.h>

using namespace rain;
using async::Task;
using rain::test::Context;

auto main() -> int
{
    Context ctx("fs");

    // fs 走 spawn_blocking → 需 enable_blocking（默认 true）安装 g_thread_pool
    auto rt_result = runtime::Runtime::builder().worker_threads(1).blocking_threads(2).build();
    TK_CHECK(ctx, rt_result.has_value());
    if (!rt_result)
        return ctx.summary();
    auto rt = std::move(*rt_result);

    const String path = "/tmp/rain_fs_test_" + std::to_string(::getpid());
    const String content = "hello rain fs";

    // case 1: write → metadata → read 往返 → exists → remove → exists=false
    {
        auto r = rt.block_on([path, content]() -> Task<Result<Unit>> {
            auto w = co_await fs::write_string(path, content);
            if (!w)
                co_return Err(std::move(w).error());

            auto m = co_await fs::metadata(path);
            if (!m)
                co_return Err(std::move(m).error());
            if (m->size != content.size() || !m->is_file())
                co_return Err(SystemError { .code = std::make_error_code(std::errc::io_error),
                                            .message = "metadata mismatch",
                                            .location = std::source_location::current() });

            auto rd = co_await fs::read_to_string(path);
            if (!rd)
                co_return Err(std::move(rd).error());
            if (*rd != content)
                co_return Err(SystemError { .code = std::make_error_code(std::errc::io_error),
                                            .message = "read content mismatch",
                                            .location = std::source_location::current() });

            auto e1 = co_await fs::exists(path);
            if (!e1 || !*e1)
                co_return Err(SystemError { .code = std::make_error_code(std::errc::io_error),
                                            .message = "exists should be true",
                                            .location = std::source_location::current() });

            auto rm = co_await fs::remove_file(path);
            if (!rm)
                co_return Err(std::move(rm).error());

            auto e2 = co_await fs::exists(path);
            if (!e2 || *e2)
                co_return Err(SystemError { .code = std::make_error_code(std::errc::io_error),
                                            .message = "exists should be false after remove",
                                            .location = std::source_location::current() });
            co_return Ok();
        }());
        TK_CHECK(ctx, r.has_value());
    }

    // case 2: 错误路径——读不存在的文件 → Err（验证 spawn_blocking 跨线程错误传播）
    {
        const String missing = "/tmp/rain_fs_missing_" + std::to_string(::getpid());
        auto r = rt.block_on([missing]() -> Task<Result<String>> {
            co_return co_await fs::read_to_string(missing);
        }());
        TK_CHECK(ctx, !r.has_value()); // 应失败，错误从 worker 线程穿回事件循环
    }

    return ctx.summary();
}
