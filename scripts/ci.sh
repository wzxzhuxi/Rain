#!/usr/bin/env bash
# Rain 的 CI / 本地共用构建+测试脚本。
# 设计意图:CI 与本地都在 Dockerfile.ci 的镜像里执行「完全相同」的这一个脚本,
# 让「本地能过」⟺「CI 能过」由构造保证,而不是靠环境碰巧一致。
#
# 用法(在仓库根目录,镜像内):
#   bash scripts/ci.sh <debug|asan|tsan|fuzz|bench|soak>

set -euo pipefail

variant="${1:?用法: ci.sh <debug|asan|tsan|fuzz|bench|soak>}"
B=/tmp/build   # 在容器内构建,不污染挂载进来的源码目录

build_tests() {
    cmake -S . -B "$B" -DCMAKE_CXX_COMPILER=g++-14 -DRAIN_BUILD_TESTS=ON "$@"
    cmake --build "$B" -j
}

case "$variant" in
  debug)
    build_tests
    ctest --test-dir "$B" --output-on-failure
    ;;

  asan)
    build_tests -DRAIN_SANITIZE=address+undefined
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        ctest --test-dir "$B" --output-on-failure
    ;;

  tsan)
    build_tests -DRAIN_SANITIZE=thread
    TSAN_OPTIONS=halt_on_error=1 ctest --test-dir "$B" --output-on-failure
    ;;

  soak)
    build_tests -DRAIN_SANITIZE=thread
    RAIN_SOAK_ITERS="${RAIN_SOAK_ITERS:-8000}" TSAN_OPTIONS=halt_on_error=1 \
        ctest --test-dir "$B" -R soak --output-on-failure
    ;;

  fuzz)
    # libFuzzer 谐振器必须用 Clang。注意:Clang 18 把 __cpp_concepts 报成旧值 201907L,
    # 而 GCC 14 的 libstdc++ 把 std::expected 等 C++23 设施卡在 __cpp_concepts>=202002L,
    # 导致 <expected> 为空。Clang 19 起 __cpp_concepts=202002L,故这里固定用 clang++-19。
    cmake -S . -B "$B" -DCMAKE_CXX_COMPILER=clang++-19 -DRAIN_BUILD_FUZZERS=ON
    cmake --build "$B" -j
    "$B"/rain_address_fuzz -artifact_prefix=/tmp/ -max_total_time=60 -timeout=10
    ;;

  bench)
    cmake -S . -B "$B" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -DRAIN_BUILD_BENCHMARKS=ON
    cmake --build "$B" -j
    "$B"/rain_echo_bench & srv=$!
    sleep 1
    out=$(wrk -t2 -c64 -d8s http://127.0.0.1:7778/) || true
    kill "$srv" 2>/dev/null || true
    echo "$out"
    rps=$(echo "$out" | awk '/Requests\/sec/ {print $2}')
    echo "Requests/sec: ${rps:-0}"
    # 生成性下限:只拦灾难性退化,不追究百分比噪声。
    awk -v r="${rps:-0}" 'BEGIN { if (r+0 < 10000) { print "BELOW FLOOR (10000 req/s)"; exit 1 } }'
    ;;

  *)
    echo "未知 variant: $variant" >&2
    exit 2
    ;;
esac
