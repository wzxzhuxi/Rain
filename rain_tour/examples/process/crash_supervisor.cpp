#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <iostream>
#include <optional>
#include <ranges>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int kWorkers = 4;
constexpr int kHeartbeatMs = 350;
constexpr int kCrashAtBeat = 3;
constexpr int kRunSeconds = 3;

volatile std::sig_atomic_t g_shutdown = 0;

void on_alarm(int) noexcept { g_shutdown = 1; }

// ── 纯函数：把 waitpid 的 status 解释成一个结果值 ─────────────────
// 不碰任何 I/O —— 输入 status，输出含义。可以脱离进程独立测试。
struct Outcome {
    bool crashed; // true: 被信号杀死；false: 正常退出
    int value;    // crashed ? 信号号 : 退出码
};

auto classify(int status) -> Outcome
{
    if (WIFSIGNALED(status)) {
        return { .crashed = true, .value = WTERMSIG(status) };
    }
    return { .crashed = false, .value = WEXITSTATUS(status) };
}

// ── 纯函数：在 pid 表里找某个 pid 的槽位，找不到返回 nullopt ──────
auto find_slot(const std::array<pid_t, kWorkers>& pids, pid_t pid) -> std::optional<std::size_t>
{
    const auto it = std::ranges::find(pids, pid);
    if (it == pids.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - pids.begin());
}

// ── 副作用：worker 心跳循环（崩溃发生在这里）─────────────────────
// crash_at_beat 用 optional 表达「是否、以及第几拍崩溃」，取代魔数 -1。
[[noreturn]] void worker_loop(int id, std::optional<int> crash_at_beat)
{
    for (int beat = 1;; ++beat) {
        std::cout << std::format("  [worker {} pid={}] 心跳 {}\n", id, getpid(), beat);

        if (crash_at_beat && beat >= *crash_at_beat) {
            std::cout << std::format("  [worker {} pid={}] 即将解引用空指针...\n", id, getpid());
            volatile int* p = nullptr;
            *p = 42; // SIGSEGV：只杀死本进程，不影响其他 worker 和 supervisor
        }

        const timespec ts { .tv_sec = kHeartbeatMs / 1000, .tv_nsec = (kHeartbeatMs % 1000) * 1'000'000L };
        nanosleep(&ts, nullptr);
    }
}

// ── 副作用：fork 一个 worker，返回子进程 pid ─────────────────────
auto spawn_worker(int slot, std::optional<int> crash_at_beat) -> pid_t
{
    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << std::format("fork 失败: {}\n", std::strerror(errno));
        std::exit(1);
    }
    if (pid == 0) {
        worker_loop(slot, crash_at_beat); // 不返回；被 SIGTERM 时由默认行为终止
    }
    return pid;
}

} // namespace

/*
 * 崩溃容错 supervisor —— 进程隔离的核心价值演示（C++ / 函数式风格）。
 *
 * supervisor fork 出 kWorkers 个 worker，其中一个会在第 3 拍故意段错误。
 * supervisor 在 waitpid 循环里检测到子进程被信号杀死（WIFSIGNALED），
 * 立刻重启一个健康的 worker 顶替它 —— 其余 worker 全程毫发无损。
 *
 * 函数式要点：把「解释 wait status」(classify) 和「定位槽位」(find_slot)
 * 抽成纯函数，副作用（fork / waitpid / 打印）留在 main 的边缘。
 * 纯函数部分输入定输出、可单测，正是「core 纯、shell 脏」的分层。
 *
 * 这是进程独有、线程/协程给不了的东西：崩溃边界。
 * 线程崩溃会带走整个进程；而一个 worker 进程段错误，邻居安然无恙。
 * 这正是 Nginx master/worker、systemd、Erlang/OTP supervisor 的根基。
 *
 * 反过来也解释了 Rain 的取舍：Rain 用线程 + 协程跑每核 EventLoop，
 * 放弃了这层崩溃隔离，换来共享地址空间和零 IPC 成本的性能。
 *
 * 测试：make run-supervisor
 */
int main()
{
    // 关掉 core dump，避免 worker 段错误时留下 core 文件
    const rlimit no_core { .rlim_cur = 0, .rlim_max = 0 };
    setrlimit(RLIMIT_CORE, &no_core);

    // 用 sigaction 安装 SIGALRM 且不设 SA_RESTART —— 保证它能打断阻塞的
    // waitpid 返回 EINTR，从而优雅收工。std::signal 在 glibc 上默认带
    // SA_RESTART，会自动重启 waitpid，导致收不到中断、永远退不出循环。
    struct sigaction sa {};
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, nullptr);

    std::cout << std::unitbuf; // 每次输出即刷新，让交错的心跳及时可见

    std::cout << std::format("[supervisor pid={}] 派生 {} 个 worker（worker 1 会在第 {} 拍崩溃）\n",
        getpid(), kWorkers, kCrashAtBeat);

    // 声明式地把每个槽位映射成一个 worker pid（只有 slot 1 会崩溃）
    std::array<pid_t, kWorkers> pids {};
    for (const int slot : std::views::iota(0, kWorkers)) {
        const auto crash = (slot == 1) ? std::optional { kCrashAtBeat } : std::nullopt;
        pids[static_cast<std::size_t>(slot)] = spawn_worker(slot, crash);
    }

    alarm(kRunSeconds);
    int restarts = 0;

    while (g_shutdown == 0) {
        int status = 0;
        const pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno == EINTR) {
                continue; // 被 SIGALRM 打断，回到 while 检查 g_shutdown
            }
            break;
        }

        const auto slot = find_slot(pids, pid);
        if (!slot) {
            continue;
        }

        if (const Outcome outcome = classify(status); outcome.crashed) {
            std::cout << std::format("[supervisor] worker {} (pid={}) 被信号 {} ({}) 杀死 → 重启\n",
                *slot, pid, outcome.value, strsignal(outcome.value));
            pids[*slot] = spawn_worker(static_cast<int>(*slot), std::nullopt); // 重启健康 worker
            ++restarts;
        } else {
            std::cout << std::format("[supervisor] worker {} (pid={}) 正常退出 code={}\n",
                *slot, pid, outcome.value);
            pids[*slot] = 0;
        }
    }

    std::cout << std::format("[supervisor] {} 秒到，关闭所有 worker\n", kRunSeconds);
    for (const pid_t pid : pids) {
        if (pid > 0) {
            kill(pid, SIGTERM);
        }
    }
    for (const pid_t pid : pids) {
        if (pid > 0) {
            waitpid(pid, nullptr, 0);
        }
    }

    std::cout << std::format("[supervisor] 共处理 {} 次崩溃重启，其余 worker 全程存活\n", restarts);
    return 0;
}
