cmake_minimum_required(VERSION 3.28)
project(Rain LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 兜底:某些「CMake 版本早于编译器版本」的组合(如 Ubuntu CMake 3.28 + Clang 18)
# 无法把 CMAKE_CXX_STANDARD=23 映射成 -std,会回退到编译器默认的 C++17,导致
# std::expected 等 C++23 库设施不可见。显式补一个 -std=c++23 兜底——已正确下发
# 时只是无害的重复,生效的仍是 c++23。
add_compile_options(-std=c++23)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Core header-only library ──────────────────────────────────────
add_library(rain_core INTERFACE)
target_include_directories(rain_core INTERFACE src)
find_package(Threads REQUIRED)
target_link_libraries(rain_core INTERFACE Threads::Threads)

# ── Options ───────────────────────────────────────────────────────
option(RAIN_BUILD_BENCHMARKS "Build benchmark executables" OFF)
option(RAIN_BUILD_TESTS "Build test executables" OFF)

# ── Sanitizers (apply to every target defined below) ──────────────
# 用法：-DRAIN_SANITIZE=address+undefined  或  -DRAIN_SANITIZE=thread
set(RAIN_SANITIZE "" CACHE STRING "Sanitizers: 'address+undefined' | 'thread' | ''")
if(RAIN_SANITIZE STREQUAL "address+undefined")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
elseif(RAIN_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

# ── Compile check ─────────────────────────────────────────────────
add_executable(rain_compile_check test/compile_check.cpp)
target_link_libraries(rain_compile_check PRIVATE rain_core)

# ── Benchmarks ────────────────────────────────────────────────────
if(RAIN_BUILD_BENCHMARKS)
    add_executable(rain_echo_bench benchmark/echo_bench.cpp)
    target_link_libraries(rain_echo_bench PRIVATE rain_core)
endif()

# ── Tests (ctest) ─────────────────────────────────────────────────
if(RAIN_BUILD_TESTS)
    enable_testing()
    foreach(t timer reactor channel runtime signal property soak sim cancel net fs lifetime graceful)
        add_executable(rain_${t}_test test/${t}_test.cpp)
        target_link_libraries(rain_${t}_test PRIVATE rain_core)
        target_include_directories(rain_${t}_test PRIVATE test)
        add_test(NAME ${t} COMMAND rain_${t}_test)
    endforeach()
endif()

# ── Fuzzers (libFuzzer; Clang only) ───────────────────────────────
option(RAIN_BUILD_FUZZERS "Build libFuzzer harnesses (Clang only)" OFF)
if(RAIN_BUILD_FUZZERS)
    add_executable(rain_address_fuzz test/address_fuzz.cpp)
    target_link_libraries(rain_address_fuzz PRIVATE rain_core)
    target_compile_options(rain_address_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -g)
    target_link_options(rain_address_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
