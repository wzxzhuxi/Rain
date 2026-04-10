cmake_minimum_required(VERSION 3.28)
project(Rain LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Core header-only library ──────────────────────────────────────
add_library(rain_core INTERFACE)
target_include_directories(rain_core INTERFACE src)
find_package(Threads REQUIRED)
target_link_libraries(rain_core INTERFACE Threads::Threads)

# ── Options ───────────────────────────────────────────────────────
option(RAIN_BUILD_BENCHMARKS "Build benchmark executables" OFF)
option(RAIN_BUILD_TESTS "Build test executables" OFF)

# ── Compile check ─────────────────────────────────────────────────
add_executable(rain_compile_check test/compile_check.cpp)
target_link_libraries(rain_compile_check PRIVATE rain_core)

# ── Benchmarks ────────────────────────────────────────────────────
if(RAIN_BUILD_BENCHMARKS)
    add_executable(rain_echo_bench benchmark/echo_bench.cpp)
    target_link_libraries(rain_echo_bench PRIVATE rain_core)
endif()
