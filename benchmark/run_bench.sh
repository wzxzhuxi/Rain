#!/bin/bash
# Rain vs Tokio Echo Benchmark
#
# 前置:
#   - wrk 已安装
#   - cmake -B build-release -DCMAKE_BUILD_TYPE=Release -DRAIN_BUILD_BENCHMARKS=ON && cmake --build build-release
#   - cd benchmark/tokio_echo && cargo build --release

set -e

WRK_THREADS=4
WRK_CONNS=256
WRK_DURATION=15s

RAIN_BIN="./build-release/rain_echo_bench"
TOKIO_BIN="./benchmark/tokio_echo/target/release/tokio_echo"

RAIN_PORT=7778
TOKIO_PORT=7779

echo "================================"
echo " Rain vs Tokio Echo Benchmark"
echo " wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${WRK_DURATION}"
echo "================================"
echo ""

# --- Rain ---
echo "[Rain] starting on :${RAIN_PORT}..."
$RAIN_BIN &
RAIN_PID=$!
sleep 1

echo "[Rain] benchmarking..."
wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${WRK_DURATION} http://127.0.0.1:${RAIN_PORT}/ 2>&1 | tee /tmp/rain_bench.txt
echo ""

kill $RAIN_PID 2>/dev/null || true
wait $RAIN_PID 2>/dev/null || true
sleep 1

# --- Tokio ---
if [ -f "$TOKIO_BIN" ]; then
    echo "[Tokio] starting on :${TOKIO_PORT}..."
    $TOKIO_BIN &
    TOKIO_PID=$!
    sleep 1

    echo "[Tokio] benchmarking..."
    wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${WRK_DURATION} http://127.0.0.1:${TOKIO_PORT}/ 2>&1 | tee /tmp/tokio_bench.txt
    echo ""

    kill $TOKIO_PID 2>/dev/null || true
    wait $TOKIO_PID 2>/dev/null || true
else
    echo "[Tokio] binary not found: $TOKIO_BIN"
    echo "  cd benchmark/tokio_echo && cargo build --release"
fi

echo "================================"
echo " done"
echo "================================"
