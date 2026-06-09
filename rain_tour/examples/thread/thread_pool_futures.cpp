#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <format>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

// 带返回值的通用作业线程池：
//   - submit(f, args...) 返回 std::future<R>，可以取回作业结果；
//   - 析构走优雅排空（drain）：把队列里剩余作业跑完再 join。
//
// 线程池天然是有可变状态的「脏壳」（mutex + 队列），把它封装成一个对象，
// 让外部代码可以用纯函数 + 声明式的方式使用它（见 main）。
class ThreadPool {
public:
    explicit ThreadPool(std::size_t workers)
    {
        for (auto i : std::views::iota(std::size_t { 0 }, workers)) {
            threads_.emplace_back([this, i] { worker_loop(i); });
        }
    }

    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool&) = delete;
    auto operator=(const ThreadPool&) -> ThreadPool& = delete;

    // 高阶函数：接收任意可调用对象，返回 future 以异步取回结果。
    template <typename F, typename... Args>
    [[nodiscard]] auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
    {
        using R = std::invoke_result_t<F, Args...>;

        // packaged_task 把「调用 + 把结果写进 future」打包成一个 void() 作业
        auto task = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<R> fut = task->get_future();

        {
            const std::lock_guard lock(mutex_);
            if (stopping_) {
                throw std::runtime_error("submit on stopped pool");
            }
            jobs_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    // 优雅关闭：不再接受新作业，跑完队列里剩余作业后 join 所有 worker。
    void shutdown()
    {
        {
            const std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& th : threads_) {
            if (th.joinable()) {
                th.join();
            }
        }
    }

private:
    void worker_loop(std::size_t worker_id)
    {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                // 关键：stopping 后仍要把队列排空，而不是立刻退出
                if (jobs_.empty()) {
                    if (stopping_) {
                        return;
                    }
                    continue;
                }
                job = std::move(jobs_.front());
                jobs_.pop();
            }
            std::cout << std::format("  worker {} 执行作业\n", worker_id);
            job();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
};

/*
 * 通用作业线程池（C++ / 函数式风格）。
 *
 * 池本身是命令式的「脏壳」（固定 worker + 共享队列 + 条件变量），
 * 但对外暴露的是函数式接口：submit 是高阶函数、返回 future。
 * main 里用纯函数 square + ranges 声明式地提交并收集结果，全程不写裸循环索引。
 *
 * 相比 thread_pool.cpp（最小版），这里多了两个生产级要点：
 *   1. submit 用 packaged_task 返回 std::future<R>，可以取回作业返回值；
 *   2. 析构走优雅排空：先置 stopping_、唤醒全部 worker，跑完剩余作业再 join。
 *
 * 这对应 Rain runtime 里 ThreadPool 的角色：spawn_blocking 把阻塞作业
 * 卸载到这种池子，再用结果通道 / eventfd 把结果送回事件循环（见 rain_15）。
 */
int main()
{
    ThreadPool pool(3);

    // 纯函数：作业逻辑。相同输入恒定相同输出（sleep 只是示意耗时）。
    constexpr auto square = [](int x) noexcept {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return x * x;
    };

    // 声明式：为 1..8 每个数提交一个作业，收集成 future 向量
    auto futures = std::views::iota(1, 9)
        | std::views::transform([&](int i) { return pool.submit(square, i); })
        | std::ranges::to<std::vector>();

    // future.get() 阻塞直到对应作业完成；逐个取回并打印
    for (auto&& [idx, fut] : std::views::enumerate(futures)) {
        std::cout << std::format("作业 {} => {}\n", idx + 1, fut.get());
    }

    // 已经 get 过的 future 不能再取，这里重新提交一轮用 fold 求平方和
    auto squares = std::views::iota(1, 9) | std::views::transform([](int x) { return x * x; });
    const long long sum = std::ranges::fold_left(squares, 0LL, std::plus<> {});
    std::cout << std::format("平方和 = {}\n", sum);
    // pool 析构时优雅排空（此处队列已空）
}
