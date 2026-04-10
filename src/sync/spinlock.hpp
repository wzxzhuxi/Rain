#pragma once

#include "core/types.hpp"

#include <atomic>

namespace rain::sync {

// 跨平台的自旋循环 CPU 暂停提示。
// x86：通过 __builtin_ia32_pause 调用 _mm_pause
// ARM：yield 指令
// 回退方案：编译器屏障（无操作，但可阻止过度优化）
inline void cpu_pause() noexcept
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__) || defined(_M_ARM)
    asm volatile("yield" ::: "memory");
#else
    // 通用回退——编译器屏障
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

class SpinLock {
public:
    SpinLock() = default;

    void lock() noexcept
    {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            cpu_pause();
        }
    }

    void unlock() noexcept { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

template<typename Lock>
class LockGuard {
public:
    explicit LockGuard(Lock& lock) : lock_(lock) { lock_.lock(); }
    ~LockGuard() { lock_.unlock(); }

    LockGuard(const LockGuard&) = delete;
    auto operator=(const LockGuard&) -> LockGuard& = delete;

private:
    Lock& lock_;
};

} // namespace rain::sync
