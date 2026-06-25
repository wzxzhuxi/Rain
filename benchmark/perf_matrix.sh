#!/usr/bin/env bash
# Rain 性能测试矩阵 —— 严谨可复现的 echo 压测驱动。
#
# 相比 run_bench.sh 的改进:
#   1. 线程对等:Tokio 默认 #[tokio::main] 起「逻辑核数」个 worker(本机 12),
#      而 Rain 硬编码 4。fair 场景用 TOKIO_WORKER_THREADS=4 拉平,才是「每核效率」对比。
#   2. 核隔离:server 钉在 4 个独立物理核(cpu 0,2,4,6,SMT 兄弟空闲),
#      wrk 钉在另外 2 个物理核(cpu 8-11),压测端永不抢占 server 的 CPU。
#   3. 每个测点先 warmup(powersave governor 下让频率爬升)再测,丢弃热身数据。
#   4. 连接数扫描 + 延迟分位(--latency)+ socket 错误,全量原始输出落盘。
#
# 进程管理(踩过的坑,务必遵守):
#   - 严禁 `pkill -f <名字>`:当前 shell 的 argv 含脚本文本,会自杀(exit 144)。
#   - 严禁 `eval "..." &`:$! 抓到的是子 shell,kill 杀不到真正的 server,
#     会泄漏僵尸(Tokio 无 REUSEPORT → 下一轮 bind 失败 panic;Rain 有 REUSEPORT → 静默堆叠)。
#   - 正解:`taskset -c .. env .. "$bin" & PID=$!`(taskset/env 都 exec,PID 不变),
#     外加 reap_all() 按精确进程名(pgrep -x,不自杀)兜底清场。
#
# 用法: bash benchmark/perf_matrix.sh   (需要 wrk;二进制需已 release 构建)
set -uo pipefail

RAIN_BIN="./build-release/rain_echo_bench"
TOKIO_BIN="./benchmark/tokio_echo/target/release/tokio_echo"
RAIN_PORT=7778
TOKIO_PORT=7779

FAIR_CPUS="0,2,4,6"       # 4 个独立物理核(SMT 兄弟空闲)
SHIP_CPUS="0-11"          # 全核,等价于不限制
WRK_CPUS="8,9,10,11"      # 物理核 4-5,与 FAIR_CPUS 完全不相交
WRK_THREADS=4
WARMUP=6
DURATION=12

OUT=/tmp/rain_perf
mkdir -p "$OUT"
SUMMARY="$OUT/summary.csv"
echo "scenario,runtime,server_threads,pinned,conns,rps,transfer,p50,p99,max_lat,sock_err,non2xx,total_req" > "$SUMMARY"

SERVER_PID=""
reap_all() {  # 按精确进程名清场——comm 是 rain_echo_bench/tokio_echo,绝不匹配 shell 自身
    local p
    for p in $(pgrep -x rain_echo_bench) $(pgrep -x tokio_echo); do kill "$p" 2>/dev/null; done
    SERVER_PID=""
}
trap reap_all EXIT

wait_port_up()   { for _ in $(seq 1 30); do curl -s -o /dev/null "http://127.0.0.1:$1/" && return 0; sleep 0.1; done; return 1; }
wait_port_down() { for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$1/" || return 0; sleep 0.1; done; return 1; }

parse_into_summary() {  # $1=file  $2=空格分隔前缀
    local f="$1"; shift
    awk -v pre="$*" '
        /Requests\/sec/    { rps=$2 }
        /Transfer\/sec/    { xfer=$2 }
        /Socket errors/    { gsub(/[^0-9 ]/,""); split($0,a," "); se=a[1]+a[2]+a[3]+a[4] }
        /Non-2xx/          { non2xx=$(NF) }
        /requests in/      { total=$1 }
        /^[[:space:]]*50%/ { p50=$2 }
        /^[[:space:]]*99%/ { p99=$2 }
        $1=="Latency" && NF>=5 { maxl=$4 }
        END {
            gsub(/ /,",",pre)
            printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n", pre,
                   (rps?rps:0),(xfer?xfer:"-"),(p50?p50:"-"),(p99?p99:"-"),
                   (maxl?maxl:"-"),(se?se:0),(non2xx?non2xx:0),(total?total:0)
        }' "$f" >> "$SUMMARY"
}

run_one() {  # scenario runtime threads pinned conns bin port cpus envassign
    local scen="$1" rt="$2" thr="$3" pin="$4" conns="$5" bin="$6" port="$7" cpus="$8" envassign="$9"
    local tag="${scen}_${rt}_c${conns}"
    local log="$OUT/${tag}.server.log"
    echo ">>> [$scen] $rt threads=$thr pinned=$pin conns=$conns"

    reap_all; wait_port_down "$port" >/dev/null   # 确保干净起点

    # 直接启动:taskset 与 env 都会 exec 替换自身,$! 即为最终 server 进程
    taskset -c "$cpus" env $envassign "$bin" >"$log" 2>&1 &
    SERVER_PID=$!

    if ! wait_port_up "$port"; then
        echo "    !! server 未就绪 / bind 失败,作废"; sed 's/^/       /' "$log"
        echo "$scen,$rt,$thr,$pin,$conns,SERVER_DOWN,-,-,-,-,-,-,-" >> "$SUMMARY"; reap_all; return
    fi
    if grep -qiE 'panic|AddrInUse' "$log"; then
        echo "    !! server 启动报错,作废"; sed 's/^/       /' "$log"
        echo "$scen,$rt,$thr,$pin,$conns,SERVER_PANIC,-,-,-,-,-,-,-" >> "$SUMMARY"; reap_all; return
    fi
    # 断言:此刻只有 1 个目标进程在跑(无泄漏堆叠)
    local nproc_run; nproc_run=$(pgrep -xc "$([ "$rt" = rain ] && echo rain_echo_bench || echo tokio_echo)")
    if [ "$nproc_run" != "1" ]; then
        echo "    !! 检测到 $nproc_run 个 $rt 实例(应为 1),作废"
        echo "$scen,$rt,$thr,$pin,$conns,LEAK_$nproc_run,-,-,-,-,-,-,-" >> "$SUMMARY"; reap_all; return
    fi

    taskset -c "$WRK_CPUS" wrk -t"$WRK_THREADS" -c"$conns" -d"${WARMUP}s" "http://127.0.0.1:$port/" >/dev/null 2>&1
    taskset -c "$WRK_CPUS" wrk -t"$WRK_THREADS" -c"$conns" -d"${DURATION}s" --latency "http://127.0.0.1:$port/" >"$OUT/${tag}.txt" 2>&1

    parse_into_summary "$OUT/${tag}.txt" "$scen $rt $thr $pin $conns"
    grep -E 'Requests/sec|Socket errors|Non-2xx' "$OUT/${tag}.txt" | sed 's/^/    /'
    reap_all; wait_port_down "$port" >/dev/null
}

echo "############ Scenario A: fair + pinned (各 4 物理核, wrk 独占 cpu 8-11) ############"
for c in 64 128 256 512 1000; do
    run_one A rain  4 yes "$c" "$RAIN_BIN"  "$RAIN_PORT"  "$FAIR_CPUS" ""
    run_one A tokio 4 yes "$c" "$TOKIO_BIN" "$TOKIO_PORT" "$FAIR_CPUS" "TOKIO_WORKER_THREADS=4"
done

echo "############ Scenario B: as-shipped (Rain4 vs Tokio12, 全核不钉) ############"
for c in 256 512; do
    run_one B rain  4  no "$c" "$RAIN_BIN"  "$RAIN_PORT"  "$SHIP_CPUS" ""
    run_one B tokio 12 no "$c" "$TOKIO_BIN" "$TOKIO_PORT" "$SHIP_CPUS" "TOKIO_WORKER_THREADS=12"
done

echo "############ DONE ############"
column -t -s, "$SUMMARY"
