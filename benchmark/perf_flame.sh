#!/usr/bin/env bash
# Rain echo_bench on-CPU 火焰图采集。
#
# 前置:带符号+frame pointer 的二进制(否则栈展开断裂):
#   cmake -B build-flame -DCMAKE_BUILD_TYPE=Release -DRAIN_BUILD_BENCHMARKS=ON \
#         -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer -g" && cmake --build build-flame -j
#   并需要 ~/FlameGraph(brendangregg/FlameGraph)。
#
# 限制:perf_event_paranoid=2 且无 root → 只能采用户态栈,内核网络栈(epoll/read/write
#       的内核部分)显示为 [unknown]。要完整栈:sudo sysctl kernel.perf_event_paranoid=1。
#
# 进程管理同 perf_matrix.sh:显式 PID + reap by 精确名,严禁 pkill -f(自杀)。
set -uo pipefail
BIN=./build-flame/rain_echo_bench
PORT=7778
OUT="${RAIN_PERF_OUT:-/tmp/rain_perf}"   # 可用 RAIN_PERF_OUT 重定向到不被 /tmp 清理的目录
DATA=$OUT/rain.perf.data
FG="$HOME/FlameGraph"
SERVER_CPUS="0,2,4,6"
WRK_CPUS="8,9,10,11"
CONNS=256
SAMPLE_SECS=20
mkdir -p "$OUT"

reap() { local p; for p in $(pgrep -x rain_echo_bench); do kill "$p" 2>/dev/null; done; }
trap reap EXIT
up() { for _ in $(seq 1 30); do curl -s -o /dev/null "http://127.0.0.1:$PORT/" && return 0; sleep 0.1; done; return 1; }

reap
taskset -c "$SERVER_CPUS" "$BIN" >"$OUT/flame_server.log" 2>&1 &
SRV=$!
up || { echo "!! server 未起来"; exit 1; }
echo "server pid=$SRV (cores $SERVER_CPUS)"

# warmup:powersave 下让频率爬升,丢弃
taskset -c "$WRK_CPUS" wrk -t4 -c"$CONNS" -d6s "http://127.0.0.1:$PORT/" >/dev/null 2>&1

# 正式负载(后台,时长 > 采样窗口)
taskset -c "$WRK_CPUS" wrk -t4 -c"$CONNS" -d$((SAMPLE_SECS + 5))s "http://127.0.0.1:$PORT/" >"$OUT/flame_wrk.txt" 2>&1 &
WRK=$!

# 采样:999Hz,fp 调用图,只采 server 的 4 个 worker 线程
perf record -F 999 --call-graph fp -p "$SRV" -o "$DATA" -- sleep "$SAMPLE_SECS" 2>"$OUT/perf_rec.log"
echo "perf record 完成"

wait "$WRK" 2>/dev/null
reap

# 渲染(--no-inline:否则 addr2line 解 inline 会卡死)
perf script --no-inline -i "$DATA" 2>/dev/null > "$OUT/rain.perf.script"
"$FG/stackcollapse-perf.pl" "$OUT/rain.perf.script" 2>/dev/null > "$OUT/rain.folded"
"$FG/flamegraph.pl" \
    --title "Rain echo_bench — on-CPU flame graph (4 cores pinned, c=${CONNS})" \
    --subtitle "perf -F999 fp; user-space only (perf_event_paranoid=2)" \
    --width 1600 --colors hot "$OUT/rain.folded" > "$OUT/rain_flame.svg"

echo "=== 采样量 ==="
echo "perf script 行数: $(wc -l <"$OUT/rain.perf.script")"
echo "folded 栈数:      $(wc -l <"$OUT/rain.folded")"
echo "wrk 吞吐:         $(awk '/Requests\/sec/{print $2}' "$OUT/flame_wrk.txt")"
echo "=== SVG ==="
ls -la "$OUT/rain_flame.svg"; grep -c '<svg' "$OUT/rain_flame.svg" >/dev/null && echo "SVG 有效"
echo "=== Top 自身占用(叶子函数,前 25) ==="
"$FG/stackcollapse-perf.pl" "$OUT/rain.perf.script" 2>/dev/null \
  | awk '{c=$NF; n=split($1,a,";"); leaf[a[n]]+=c} END{for(k in leaf) print leaf[k], k}' \
  | sort -rn | head -25
echo "DONE_FLAME"
