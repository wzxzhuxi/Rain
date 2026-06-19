#pragma once

// Rain 测试工具——极简断言框架，无第三方依赖，契合 header-only 风格。
// 每个测试可执行文件持有一个 Context，用 TK_CHECK / TK_CHECK_EQ 累计结果，
// main 末尾 return ctx.summary()。非零退出码即失败，由 CTest 捕获。
//
// 这些测试的真正校验由 sanitizer 承担：在 ASan/UBSan/TSan 下运行时，
// 生命周期/竞争类 bug 会直接变成进程级失败，断言只覆盖可观察的行为正确性。

#include <cstdio>

namespace rain::test {

class Context {
public:
    explicit Context(const char* name) : name_(name) { }

    auto check(bool ok, const char* expr, const char* file, int line) -> void
    {
        ++total_;
        if (!ok) {
            ++failed_;
            std::fprintf(stderr, "  [FAIL] %s:%d  %s\n", file, line, expr);
        }
    }

    template<typename A, typename B>
    auto check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) -> void
    {
        ++total_;
        if (!(a == b)) {
            ++failed_;
            std::fprintf(stderr, "  [FAIL] %s:%d  %s == %s\n", file, line, ea, eb);
        }
    }

    [[nodiscard]] auto summary() const -> int
    {
        std::fprintf(stderr, "[%s] %d/%d checks passed%s\n", name_, total_ - failed_, total_,
                     failed_ == 0 ? "" : "  *** FAILURES ***");
        return failed_ == 0 ? 0 : 1;
    }

private:
    const char* name_;
    int total_ = 0;
    int failed_ = 0;
};

} // namespace rain::test

#define TK_CHECK(ctx, cond) (ctx).check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define TK_CHECK_EQ(ctx, a, b) (ctx).check_eq((a), (b), #a, #b, __FILE__, __LINE__)
