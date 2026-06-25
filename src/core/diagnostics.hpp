#pragma once

// ── 可观测性 seam（Layer 0）──────────────────────────────────────────
// 结构化诊断事件 + 每核原子计数器。设计目标：
//   - header-only、关闭时（未安装 sink / 未安装 g_metrics）零开销
//   - 可注入：sink 与 metrics 由 runtime 在每核启动时安装（镜像 g_reactor）
//   - 不违反严格分层：只依赖 core/{types,result} + 标准库，不反向 include 任何上层，
//     故 io_reactor(L2)/task(L2)/runtime(L3)/net(L4) 都能用而不成环。
//
// 热路径只有 emit() 的一次 atomic load + 分支、count() 的一次 null 检查 + relaxed
// fetch_add；未安装时退化为一次分支即返回。结构化字段在栈上构造、零堆分配——热点处
// 用 `if (diag::enabled())` 包裹事件构造即可连字段构造也省掉。

#include "core/result.hpp"
#include "core/types.hpp"

#include <atomic>
#include <concepts>
#include <cstdio>
#include <mutex>
#include <source_location>
#include <span>
#include <vector>

namespace rain::diag {

enum class Severity : u8 { Trace, Debug, Info, Warn, Error };

// 结构化字段：栈上构造、零堆分配。value 是整型或 StringView，由 sink 负责格式化。
struct Field {
    enum class Kind : u8 { I64, U64, Str };
    StringView key;
    Kind kind = Kind::U64;
    i64 ival = 0;
    StringView sval {};

    [[nodiscard]] static constexpr auto i(StringView k, i64 v) -> Field { return { k, Kind::I64, v, {} }; }
    [[nodiscard]] static constexpr auto u(StringView k, u64 v) -> Field { return { k, Kind::U64, static_cast<i64>(v), {} }; }
    [[nodiscard]] static constexpr auto s(StringView k, StringView v) -> Field { return { k, Kind::Str, 0, v }; }
};

// 一条结构化事件。code 是稳定事件码（如 "loop.run_failed"），不在此预格式化。
struct Event {
    StringView code;
    Severity severity = Severity::Info;
    const SystemError* error = nullptr; // 可空，携带 errno + source_location
    std::span<const Field> fields {};
    std::source_location loc = std::source_location::current();
};

// Sink 概念：组合优于继承，镜像 ThreadPool::ExceptionHandler 的注入风格。
template<typename S>
concept Sink = requires(S& s, const Event& e) {
    { s.emit(e) } -> std::same_as<void>;
};

// 零堆分配的类型擦除：函数指针 + ctx（不用 std::function，避免热路径分配）。
// 调用方负责 SinkRef 的生命周期：install 后销毁 SinkRef 即悬空。
struct SinkRef {
    using Fn = void (*)(void*, const Event&) noexcept;
    Fn fn = nullptr;
    void* ctx = nullptr;

    auto operator()(const Event& e) const -> void { fn(ctx, e); }

    template<Sink S>
    [[nodiscard]] static auto of(S& s) noexcept -> SinkRef
    {
        return { +[](void* c, const Event& e) noexcept { static_cast<S*>(c)->emit(e); }, &s };
    }
};

// 全局已安装 sink：存「指向稳定 SinkRef 的指针」。指针宽度 atomic 必 lock-free，
// 避开 atomic<SinkRef>（16 字节）需 cmpxchg16b 的坑。
inline std::atomic<const SinkRef*> g_sink { nullptr };

inline auto install_sink(const SinkRef* s) -> void { g_sink.store(s, std::memory_order_release); }
inline auto reset_sink() -> void { g_sink.store(nullptr, std::memory_order_release); }
[[nodiscard]] inline auto enabled() noexcept -> bool { return g_sink.load(std::memory_order_relaxed) != nullptr; }

// 热路径门控：未安装 sink 时一次 acquire load + 分支即返回 —— 关闭零开销。
inline auto emit(const Event& e) -> void
{
    if (const auto* s = g_sink.load(std::memory_order_acquire))
        (*s)(e);
}

// ── 每核原子计数器 ───────────────────────────────────────────────────
// 每核单线程 relaxed 写、scrape 线程跨核读 → 必须 atomic。
struct CoreMetrics {
    std::atomic<u64> accept_failed { 0 };        // accept4 失败
    std::atomic<u64> inflight_connections { 0 }; // 在飞连接（InflightGuard +1/-1）
    std::atomic<u64> coroutine_exceptions { 0 }; // Task::unhandled_exception 命中
    std::atomic<u64> blocking_exceptions { 0 };  // ThreadPool 任务抛异常
    std::atomic<u64> reactor_rearm_failed { 0 }; // epoll 重新武装失败（fd 静默失活）
    std::atomic<u64> loop_run_errors { 0 };      // EventLoop::run() 返回 Err
};

// 每核线程局部指针：runtime 在每核启动时安装（镜像 g_reactor）。关闭(=nullptr)时增量退化成一次分支。
inline thread_local CoreMetrics* g_metrics = nullptr;

inline auto count(std::atomic<u64> CoreMetrics::* field, u64 n = 1) noexcept -> void
{
    if (auto* m = g_metrics)
        (m->*field).fetch_add(n, std::memory_order_relaxed);
}

// 在飞连接 RAII——给 accept 循环用。
struct InflightGuard {
    InflightGuard() noexcept { count(&CoreMetrics::inflight_connections); }
    ~InflightGuard()
    {
        if (auto* m = g_metrics)
            m->inflight_connections.fetch_sub(1, std::memory_order_relaxed);
    }
    InflightGuard(const InflightGuard&) = delete;
    auto operator=(const InflightGuard&) -> InflightGuard& = delete;
};

// ── 跨核 scrape 注册表 ───────────────────────────────────────────────
// 冷路径（仅注册/注销/scrape 时上锁）；emit/count 热路径不触碰它。
class MetricsRegistry {
public:
    [[nodiscard]] static auto instance() -> MetricsRegistry&
    {
        static MetricsRegistry r;
        return r;
    }

    auto add(CoreMetrics* m) -> void
    {
        std::lock_guard lk(mu_);
        cores_.push_back(m);
    }

    auto remove(CoreMetrics* m) -> void
    {
        std::lock_guard lk(mu_);
        std::erase(cores_, m);
    }

    // f(const CoreMetrics&) 在锁内被逐核调用——scrape 必须在锁内完成，
    // 否则 worker 退出注销与读取形成 use-after-free（TSan 才能抓的 race）。
    template<typename F>
    auto for_each(F&& f) -> void
    {
        std::lock_guard lk(mu_);
        for (auto* m : cores_)
            f(*m);
    }

private:
    std::mutex mu_;
    std::vector<CoreMetrics*> cores_;
};

// ── 开箱即用的 stderr sink ───────────────────────────────────────────
// 把结构化 Event 格式化到 stderr。默认不安装（保持零开销/可注入），用 ScopedSink 一行开启。
struct StderrSink {
    auto emit(const Event& e) -> void
    {
        static constexpr const char* kSev[] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR" };
        std::fprintf(stderr, "[%s] %.*s", kSev[static_cast<int>(e.severity)], static_cast<int>(e.code.size()),
            e.code.data());
        if (e.error != nullptr)
            std::fprintf(stderr, " errno=%d msg=%s", e.error->code.value(), e.error->message.c_str());
        for (const auto& f : e.fields) {
            std::fprintf(stderr, " %.*s=", static_cast<int>(f.key.size()), f.key.data());
            if (f.kind == Field::Kind::Str)
                std::fprintf(stderr, "%.*s", static_cast<int>(f.sval.size()), f.sval.data());
            else
                std::fprintf(stderr, "%lld", static_cast<long long>(f.ival));
        }
        std::fprintf(stderr, "\n");
    }
};

// RAII sink 安装守卫：作用域内安装 S 为全局 sink，析构时复位。用法：
//   diag::ScopedSink<diag::StderrSink> log_guard;  // 本作用域内 diag::emit 输出到 stderr
template<Sink S>
class ScopedSink {
public:
    ScopedSink() { install_sink(&ref_); }
    ~ScopedSink() { reset_sink(); }
    ScopedSink(const ScopedSink&) = delete;
    auto operator=(const ScopedSink&) -> ScopedSink& = delete;
    [[nodiscard]] auto sink() noexcept -> S& { return sink_; }

private:
    S sink_ {};
    SinkRef ref_ = SinkRef::of(sink_); // 成员序：sink_ 先于 ref_，ref_ 持有 &sink_
};

} // namespace rain::diag
