#pragma once

// SimReactor —— 满足 IoReactorLike 的内存态、确定性 reactor 测试替身。
// 不碰真实 fd / epoll：就绪由 make_readable / make_writable 显式注入，poll 据此触发。
//
// 它存在的意义有二：
//   1. 证明 IoReactorLike 是一条真正的抽象缝——EpollReactor 之外存在第二个合规实现，
//      意味着把上层泛化到该概念是可行的，而非偶然依赖了 EpollReactor 的具体类型。
//   2. 作为"全运行时确定性仿真"的可直接接入替身：一旦 EventLoop / awaiter 被泛化到
//      IoReactorLike + TimerLike，把 EpollReactor 换成 SimReactor + 虚拟时钟，整个 run()
//      循环就变成单线程、可复现、可注入的。
//
// 其公开面镜像 EpollReactor 中 awaiter 实际调用的部分（register_read/write、deregister、
// poll、drain_ready、add/modify/remove），因此是名副其实的 drop-in 候选。
//
// 接入全运行时仿真所需的改动（一次独立、需评审的重构，本测试只铺地基不动生产代码）：
//   - EventLoop 模板化：EventLoop<Reactor=EpollReactor, Timer=HierarchicalTimerWheel>，
//     reactor_/timer_ 改为模板成员。
//   - awaiter 泛化：net/io_awaiters.hpp 与 bridge.hpp 中持 `EpollReactor&` 的 awaiter
//     改为持 `R&`（R 满足 IoReactorLike），或统一从泛化后的 g_reactor 取。
//   - 线程局部 g_reactor/g_timer 改为模板或类型擦除（按 reactor 类型参数化）。
//   - Runtime/Executor 默认实例化 EpollReactor；测试实例化 SimReactor + 虚拟时钟。
//   风险：触及整个 async/net/runtime I/O 栈与 epoll 热路径，应作为单独 PR 评审。

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/concepts.hpp"

#include <coroutine>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rain::test {

class SimReactor {
public:
    struct Pending {
        std::coroutine_handle<> handle {};
        Result<usize>* result = nullptr;
        async::WaitState* state = nullptr;
        async::WaitState completion = async::WaitState::Ready;
    };

    // ── IoReactorLike 概念要求的接口（无真实 epoll，均为内存态） ──
    [[nodiscard]] auto add(i32, async::IoEvent) -> Result<Unit> { return Ok(); }
    [[nodiscard]] auto modify(i32, async::IoEvent) -> Result<Unit> { return Ok(); }
    [[nodiscard]] auto remove(i32 fd) -> Result<Unit>
    {
        pending_.erase(fd);
        return Ok();
    }

    [[nodiscard]] auto poll(async::Duration) -> Result<u32>
    {
        u32 fired = 0;
        for (auto& [fd, e] : pending_) {
            if (e.read && readable_.contains(fd)) {
                if (fire(*e.read))
                    ++fired;
                e.read.reset();
            }
            if (e.write && writable_.contains(fd)) {
                if (fire(*e.write))
                    ++fired;
                e.write.reset();
            }
        }
        readable_.clear();
        writable_.clear();
        std::erase_if(pending_, [](const auto& kv) { return !kv.second.read && !kv.second.write; });
        return Ok(fired);
    }

    // ── awaiter 调用面（镜像 EpollReactor） ──
    [[nodiscard]] auto register_read(i32 fd, std::coroutine_handle<> h, u8*, usize, Result<usize>* result_ptr,
                                     async::WaitState* state = nullptr,
                                     async::WaitState completion = async::WaitState::Ready) -> Result<Unit>
    {
        auto& e = pending_[fd];
        if (e.read)
            return Err(busy());
        e.read = Pending { .handle = h, .result = result_ptr, .state = state, .completion = completion };
        return Ok();
    }

    [[nodiscard]] auto register_write(i32 fd, std::coroutine_handle<> h, const u8*, usize, Result<usize>* result_ptr,
                                      async::WaitState* state = nullptr,
                                      async::WaitState completion = async::WaitState::Ready) -> Result<Unit>
    {
        auto& e = pending_[fd];
        if (e.write)
            return Err(busy());
        e.write = Pending { .handle = h, .result = result_ptr, .state = state, .completion = completion };
        return Ok();
    }

    auto deregister(i32 fd) -> Unit
    {
        pending_.erase(fd);
        return unit;
    }

    [[nodiscard]] auto drain_ready() -> std::vector<std::coroutine_handle<>> { return std::exchange(ready_, {}); }

    // ── 测试注入：声明某 fd 在下一次 poll 时可读/可写 ──
    auto make_readable(i32 fd) -> void { readable_.insert(fd); }
    auto make_writable(i32 fd) -> void { writable_.insert(fd); }

private:
    struct FdEntry {
        std::optional<Pending> read;
        std::optional<Pending> write;
    };

    static auto busy() -> SystemError
    {
        return SystemError { .code = std::make_error_code(std::errc::device_or_resource_busy),
                             .message = "SimReactor: direction already pending",
                             .location = std::source_location::current() };
    }

    auto fire(const Pending& p) -> bool
    {
        if (p.state) {
            if (*p.state != async::WaitState::Pending)
                return false;
            *p.state = p.completion;
        }
        if (p.result)
            *p.result = Ok(usize { 0 });
        if (p.handle) {
            ready_.push_back(p.handle);
            return true;
        }
        return false;
    }

    std::unordered_map<i32, FdEntry> pending_;
    std::unordered_set<i32> readable_;
    std::unordered_set<i32> writable_;
    std::vector<std::coroutine_handle<>> ready_;
};

static_assert(async::IoReactorLike<SimReactor>, "SimReactor must satisfy IoReactorLike");

} // namespace rain::test
