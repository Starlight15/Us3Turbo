#!/bin/bash
# bench_put_4mb.sh — 4MB 单步 PUT 多轮稳定测试脚本
#
# 用法:
#   ./bench_put_4mb.sh --path gds|rdma --concurrency N [--mock-aio-write 0|1]
#
# 流程:
#   1. 停止旧服务 (proxy:9100, ufile-ac:24000)
#   2. 启动 ufile-ac（可选 --mock-aio-write 覆盖）
#   3. 启动 proxy
#   4. 等待就绪
#   5. 反复跑 bench 直到连续 3 轮 ops/s 方差 < 5%
#   6. 输出稳定值
#
set -euo pipefail

# ---- 路径 ----
readonly UFILE_AC_BIN="/mnt/us3_test/xinghui.shao/gds/ggds-compile-env/ufile-ac/build/ufile-ac"
readonly UFILE_AC_CONFIG="/mnt/us3_test/xinghui.shao/gds/ggds-compile-env/ufile-ac/config/ufile-ac-gds-proxy.ini"
readonly PROXY_BIN="/mnt/us3_test/xinghui.shao/gds/Us3Turbo/build/proxy/us3_turbo_proxy"
readonly PROXY_FLAGS="/mnt/us3_test/xinghui.shao/gds/Us3Turbo/proxy/conf/proxy.flags"
readonly GDS_BENCH="/mnt/us3_test/xinghui.shao/gds/Us3Turbo/build/rtest/bench/gds/us3_turbo_bench_gds_put"
readonly RDMA_BENCH="/mnt/us3_test/xinghui.shao/gds/Us3Turbo/build/rtest/bench/rdma/us3_turbo_bench_rdma_put"
readonly UFILE_AC_LOG="/mnt/us3_test/xinghui.shao/gds/ggds-compile-env/ufile-ac/build/ufile_ac.log"
readonly PROXY_LOG="/mnt/us3_test/xinghui.shao/gds/Us3Turbo/build/proxy_out.log"

# ---- 全局参数（由 main 设置） ----
PATH_NAME=""
CONCURRENCY=""
MOCK_AIO_WRITE=""
SIZE="4M"
COUNT="100"
WARMUP="5"
MAX_ROUNDS="10"
STABLE_ROUNDS="3"
VARIANCE_THRESHOLD="5.0"
BENCH_BIN=""

usage() {
  cat <<EOF
usage: $0 --path gds|rdma --concurrency N [--mock-aio-write 0|1] [--max-rounds N]

  4MB 单步 PUT 多轮稳定测试。每次跑 $COUNT ops，取连续 $STABLE_ROUNDS 轮
  ops/s 方差 < ${VARIANCE_THRESHOLD}% 时的均值作为稳定结果。

  --path gds|rdma           数据通路（必填）
  --concurrency N           并发 worker 数（必填）
  --mock-aio-write 0|1      跳过磁盘写入: 1=skip, 0=real write（默认 1）
  --max-rounds N            最大测试轮数（默认 $MAX_ROUNDS）
  --size N[K|M|G]           对象大小（默认 $SIZE）
  --count N                 每轮对象数（默认 $COUNT）
  --warmup N                warmup 数（默认 $WARMUP）
EOF
  exit 1
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --path)           PATH_NAME="$2"; shift 2 ;;
      --concurrency)    CONCURRENCY="$2"; shift 2 ;;
      --mock-aio-write) MOCK_AIO_WRITE="$2"; shift 2 ;;
      --max-rounds)     MAX_ROUNDS="$2"; shift 2 ;;
      --size)           SIZE="$2"; shift 2 ;;
      --count)          COUNT="$2"; shift 2 ;;
      --warmup)         WARMUP="$2"; shift 2 ;;
      -h|--help)        usage ;;
      *) echo "unknown arg: $1"; usage ;;
    esac
  done

  if [[ -z "$PATH_NAME" || -z "$CONCURRENCY" ]]; then
    echo "ERROR: --path and --concurrency are required"
    usage
  fi

  if [[ "$PATH_NAME" != "gds" && "$PATH_NAME" != "rdma" ]]; then
    echo "ERROR: --path must be gds or rdma"
    usage
  fi

  MOCK_AIO_WRITE="${MOCK_AIO_WRITE:-1}"

  if [[ "$PATH_NAME" == "gds" ]]; then
    BENCH_BIN="$GDS_BENCH"
  else
    BENCH_BIN="$RDMA_BENCH"
  fi
}

stop_services() {
  fuser -k 9100/tcp 2>/dev/null || true
  fuser -k 24000/tcp 2>/dev/null || true
  sleep 1
}

start_ufile_ac() {
  local mock_flag="--mock-aio-write=$MOCK_AIO_WRITE"
  echo "[$(date '+%H:%M:%S')] Starting ufile-ac $mock_flag ..."
  nohup "$UFILE_AC_BIN" \
    --config-file="$UFILE_AC_CONFIG" \
    "$mock_flag" \
    > "$UFILE_AC_LOG" 2>&1 &
  echo "  ufile-ac pid=$!"
}

start_proxy() {
  echo "[$(date '+%H:%M:%S')] Starting proxy ..."
  nohup "$PROXY_BIN" --flagfile="$PROXY_FLAGS" \
    > "$PROXY_LOG" 2>&1 &
  echo "  proxy pid=$!"
}

wait_ready() {
  echo -n "[$(date '+%H:%M:%S')] Waiting for services..."
  local i
  for i in $(seq 1 30); do
    if ss -tlnp 2>/dev/null | grep -q ':24000' && ss -tlnp 2>/dev/null | grep -q ':9100'; then
      echo " ready (${i}s)"
      return 0
    fi
    sleep 1
    echo -n "."
  done
  echo " TIMEOUT"
  return 1
}

run_bench() {
  "$BENCH_BIN" \
    --proxy 192.168.1.198:9100 \
    --size "$SIZE" \
    --count "$COUNT" \
    --concurrency "$CONCURRENCY" \
    --warmup "$WARMUP" \
    2>&1
}

parse_ops() {
  grep -oP 'throughput\s*:\s*\K[0-9.]+(?=\s*MiB/s\s*\(\s*[0-9.]+\s*ops/s\))' || true
}

parse_mibs() {
  grep -oP 'throughput\s*:\s*\K[0-9.]+(?=\s*MiB/s)' || true
}

# 检查最后 N 轮的相对偏差是否 < 阈值
check_stable() {
  local -n arr=$1       # ops 数组引用
  local n=${#arr[@]}
  local last_n=$2       # 取最后几轮
  local threshold=$3

  if [[ $n -lt $last_n ]]; then
    return 1
  fi

  # 提取最后 last_n 个值
  local start=$((n - last_n))
  local sum=0.0
  local count=0
  local i
  local vals=()
  for ((i=start; i<n; i++)); do
    local v="${arr[$i]}"
    vals+=("$v")
    sum=$(awk "BEGIN {print $sum + $v}")
    count=$((count + 1))
  done

  local mean
  mean=$(awk "BEGIN {printf \"%.2f\", $sum / $count}")

  # 最大相对偏差 %
  local max_dev=0.0
  for v in "${vals[@]}"; do
    local dev
    dev=$(awk "BEGIN {d = ($v - $mean) / $mean; if (d < 0) d = -d; printf \"%.2f\", d * 100}")
    if [[ $(awk "BEGIN {print ($dev > $max_dev)}") == "1" ]]; then
      max_dev=$dev
    fi
  done

  echo "  last $last_n rounds: ${vals[*]}  mean=${mean}  max_dev=${max_dev}%"

  if [[ $(awk "BEGIN {print ($max_dev < $threshold)}") == "1" ]]; then
    echo ""
    echo "============================================"
    echo "STABLE (max_dev=${max_dev}% < ${threshold}%)"
    echo "  path            : $PATH_NAME"
    echo "  size            : $SIZE"
    echo "  concurrency     : $CONCURRENCY"
    echo "  mock_aio_write  : $MOCK_AIO_WRITE"
    echo "  ops/s (mean)    : $(printf "%.0f" "$mean")"
    echo "  samples         : ${vals[*]}"
    echo "============================================"
    echo "RESULT_JSON: {\"path\":\"$PATH_NAME\",\"size\":\"$SIZE\",\"concurrency\":$CONCURRENCY,\"mock_aio_write\":$MOCK_AIO_WRITE,\"ops_per_sec\":$mean,\"rounds\":$n,\"max_dev_pct\":$max_dev,\"samples\":[${vals[*]}]}"
    return 0
  fi
  return 1
}

# 计算所有轮次的均值（best-effort, 用于 max_rounds 达到但未稳定时）
best_effort_ops() {
  local -n arr=$1
  local sum=0.0
  local count=0
  local v
  for v in "${arr[@]}"; do
    sum=$(awk "BEGIN {print $sum + $v}")
    count=$((count + 1))
  done
  awk "BEGIN {printf \"%.0f\", $sum / $count}"
}

main() {
  parse_args "$@"

  echo "============================================"
  echo "bench_put_4mb: path=$PATH_NAME  size=$SIZE  concurrency=$CONCURRENCY  mock_aio_write=$MOCK_AIO_WRITE"
  echo "  count=$COUNT  warmup=$WARMUP  max_rounds=$MAX_ROUNDS"
  echo "============================================"

  stop_services
  start_ufile_ac
  start_proxy

  if ! wait_ready; then
    echo "ERROR: services did not start"
    stop_services
    exit 1
  fi

  # ---- 多轮测试直到稳定 ----
  local ops_history=()
  local r

  for ((r=0; r<MAX_ROUNDS; r++)); do
    local round=$((r + 1))
    echo ""
    echo "--- Round $round/$MAX_ROUNDS ---"

    local output ops mibs
    output=$(run_bench)
    ops=$(echo "$output" | parse_ops)
    mibs=$(echo "$output" | parse_mibs)

    if [[ -z "$ops" || "$ops" == "0" ]]; then
      echo "  FAILED to parse ops/s. Output (last 5 lines):"
      echo "$output" | tail -5
      if echo "$output" | grep -q "E12001\|backend unavailable\|failed\|Initialize"; then
        echo "  Connection error detected. Restarting services..."
        stop_services
        start_ufile_ac
        start_proxy
        sleep 3
      fi
      ops_history=()
      continue
    fi

    echo "  ops/s = $ops  MiB/s = $mibs"
    ops_history+=("$ops")

    if check_stable ops_history "$STABLE_ROUNDS" "$VARIANCE_THRESHOLD"; then
      stop_services
      return 0
    fi

    sleep 1
  done

  # 达到最大轮次仍未稳定
  echo ""
  echo "WARNING: reached max rounds ($MAX_ROUNDS) without stable results"
  if [[ ${#ops_history[@]} -gt 0 ]]; then
    local mean
    mean=$(best_effort_ops ops_history)
    echo "  best-effort mean ops/s = $mean"
    echo "  all samples: ${ops_history[*]}"
  fi

  stop_services
  return 1
}

main "$@"
