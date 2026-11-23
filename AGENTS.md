# AGENTS.md

This file provides guidance to the AI agent when working with code in this repository.

## Project

Rain is a header-only C++23 async runtime for Linux (epoll/eventfd/sendfile, pthreads). Library code lives entirely in `src/` as `.hpp` files with `#pragma once`; there are no `.cpp` library sources and no third-party dependencies. `README.md` (Chinese) and `README_EN.md` are authoritative.

## Build & verify

- Prefer the `/verify` skill for the local build+test loop.
- Use Unix Makefiles, not Ninja — CMake 3.28 + Ninja + C++23 breaks on module scanning here.
- Local: `cmake -B build -DRAIN_BUILD_TESTS=ON && cmake --build build -j && ctest --test-dir build --output-on-failure`
- Single test: `ctest --test-dir build -R <name> --output-on-failure`; names: `timer reactor channel runtime property soak sim cancel net lifetime graceful`.
- `./build/rain_compile_check` is built unconditionally and is the fastest header compile check.
- Sanitizers are the real oracle here: `-DRAIN_SANITIZE=address+undefined` or `-DRAIN_SANITIZE=thread`. Fuzzers need Clang 19 (`-DRAIN_BUILD_FUZZERS=ON`); Clang 18 hides C++23 `std::expected`.
- For Docker CI parity, ask the user to run `/ci <variant>`; variants: `debug asan tsan fuzz bench soak`.
- Static analysis: `/lint` runs clang-tidy with the root `.clang-tidy`; advisory only — CI does not run it.
- Tests use the custom `test/testkit.hpp` (`TK_CHECK`, `TK_CHECK_EQ`, `Context::summary()`), not GoogleTest/Catch2.

## Architecture

- Strict layer order: `core` → `sync` → `async` → `runtime` → `net`. Includes may only point downward; never add upward dependencies.
- `Result<T, E>` (`std::expected`) is the error path — do not introduce exception-based control flow.
- Hot paths are per-core shared-nothing; avoid adding cross-core shared state.
- When adding a public header, add its include to `test/compile_check.cpp`.

## Style

- 4-space indent, ~120 columns, Allman braces for function bodies; same-line braces for `if`/`for` and class headers. `src/core/types.hpp` is the lone 2-space file — don't reformat it.
- Trailing return types (`auto f() -> T`); mark functions `[[nodiscard]]` like the surrounding code.
- Naming: PascalCase types, snake_case functions, `trailing_` members, `kCamelCase` constants, lowercase namespaces (`rain::async`, `detail`).
- Include order: project headers (grouped by layer) first, then std, then POSIX.
- Comments are Chinese; no formatter is configured — match surrounding style by hand.

## Git

- Conventional Commits with Chinese subjects and module scopes: `feat(runtime): ...`, `fix(async): ...`, `test: ...`, `perf: ...`.
- Commit only when explicitly asked; never push or open PRs.
- `.gitignore` is whitelist mode (`*` plus explicit `!` includes) — new paths or file types are silently untracked until allowlisted.
