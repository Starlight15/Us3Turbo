#!/bin/bash
# run-rdma-bench.sh — RDMA (libibverbs) 性能基准一键运行
#
# 用法:
#   ./scripts/run-rdma-bench.sh                                    # 默认: 单步 100M, conc=1, reps=1
#   ./scripts/run-rdma-bench.sh --mode single --size 256M --conc 8 # 单步并发
#   ./scripts/run-rdma-bench.sh --mode multipart --total 256M      # 分段上传
#   ./scripts/run-rdma-bench.sh --mode multipart --total 256M --part-size 16M --conc 4 --reps 5
#   ./scripts/run-rdma-bench.sh --mode both --size 256M --total 256M --conc 1,2,4,8
#
# 依赖: 编译产物须已存在于 build/rtest/examples/ 下。
#       若不存在，脚本自动编译。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
BUILD_DIR="${PROJECT_DIR}/build"
EXAMPLE_BIN="${BUILD_DIR}/rtest/examples/rdma/us3_turbo_rdma_put_example"

# ---- 默认参数 ----
MODE="single"        # single | multipart | both
PROXY="192.168.1.198:9100"
SIZE="100M"          # 单步对象大小
TOTAL="256M"         # 分段总大小
PART_SIZE="4M"       # part 大小
CONCURRENCIES="1"    # 逗号分隔的并发列表 (如 "1,2,4,8")
REPS="5"             # 单步 reps
MP_REPS="3"          # 分段 reps
WARMUP="1"           # 分段预热轮数
VERIFY=""            # --verify-crc32c
TRACE=""             # --trace
CSV=""               # 输出 CSV

# ---- 参数解析 ----
while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      MODE="$2"; shift 2 ;;
    --proxy)
      PROXY="$2"; shift 2 ;;
    --size)
      SIZE="$2"; shift 2 ;;
    --total)
      TOTAL="$2"; shift 2 ;;
    --part-size)
      PART_SIZE="$2"; shift 2 ;;
    --conc|--concurrency)
      CONCURRENCIES="$2"; shift 2 ;;
    --reps)
      REPS="$2"; shift 2 ;;
    --mp-reps)
      MP_REPS="$2"; shift 2 ;;
    --warmup)
      WARMUP="$2"; shift 2 ;;
    --verify-crc32c)
      VERIFY="--verify-crc32c"; shift ;;
    --trace)
      TRACE="--trace"; shift ;;
    --csv)
      CSV="--csv"; shift ;;
    --help|-h)
      echo "用法: $0 [选项]"
      echo ""
      echo "模式:"
      echo "  --mode MODE          single | multipart | both (默认 single)"
      echo ""
      echo "通用:"
      echo "  --proxy ADDR         proxy 地址 (默认 192.168.1.198:9100)"
      echo "  --conc 1,2,4,8       逗号分隔的并发列表 (默认 1)"
      echo "  --verify-crc32c      开启 CRC32C 校验"
      echo "  --trace              开启 client latency_trace"
      echo ""
      echo "单步 (--mode single):"
      echo "  --size SIZE          对象大小 (默认 100M)"
      echo "  --reps N             每 worker 重复次数 (默认 5)"
      echo ""
      echo "分段 (--mode multipart):"
      echo "  --total SIZE         对象总大小 (默认 256M)"
      echo "  --part-size SIZE     part 大小 (默认 4M, <=16M)"
      echo "  --mp-reps N          每 worker 重复轮数 (默认 3)"
      echo "  --warmup N           预热轮数 (默认 1)"
      echo ""
      echo "示例:"
      echo "  $0 --mode single --size 16M --conc 1"
      echo "  $0 --mode multipart --total 256M --part-size 16M --conc 8"
      echo "  $0 --mode both --size 256M --total 256M --conc 1,2,4,8"
      exit 0 ;;
    *)
      echo "未知参数: $1"; exit 2 ;;
  esac
done

# ---- 编译检查 ----
if [[ ! -x "$EXAMPLE_BIN" ]]; then
  echo "[build] 编译 rdma_put_example..."
  cmake --build "$BUILD_DIR" -j"$(nproc)" --target us3_turbo_rdma_put_example \
    || { echo "[build] 编译失败"; exit 1; }
fi

# ---- 展开并发列表 ----
IFS=',' read -ra CONC_LIST <<< "$CONCURRENCIES"

# ---- 工具函数 ----
human_mibps() {
  local mibs="$1"
  if (( $(echo "$mibs >= 1024" | bc -l 2>/dev/null || echo 0) )); then
    printf "%.2f GiB/s" "$(echo "scale=2; $mibs / 1024" | bc)"
  else
    printf "%.1f MiB/s" "$mibs"
  fi
}

run_single() {
  local conc="$1"
  echo ""
  echo "--- 单步 PUT: size=$SIZE conc=$conc reps=$REPS ---"
  local extra_args="$VERIFY $TRACE"
  $EXAMPLE_BIN \
    --proxy "$PROXY" --size "$SIZE" \
    --concurrency "$conc" --reps "$REPS" \
    $extra_args
}

run_multipart() {
  local conc="$1"
  echo ""
  echo "--- 分段 PUT: total=$TOTAL part-size=$PART_SIZE conc=$conc reps=$MP_REPS warmup=$WARMUP ---"
  local extra_args="$VERIFY $TRACE $CSV"
  $EXAMPLE_BIN \
    --multipart --proxy "$PROXY" \
    --size "$TOTAL" --part-size "$PART_SIZE" \
    --concurrency "$conc" --reps "$MP_REPS" \
    $extra_args
}

# ---- 主流程 ----
echo "════════════════════════════════════════════════════════"
echo "  RDMA Bench"
echo "  proxy    : $PROXY"
echo "  mode     : $MODE"
echo "  conc     : $CONCURRENCIES"
echo "════════════════════════════════════════════════════════"

START_TS=$(date +%s)

for conc in "${CONC_LIST[@]}"; do
  case "$MODE" in
    single)
      run_single "$conc"
      ;;
    multipart)
      run_multipart "$conc"
      ;;
    both)
      run_single "$conc"
      run_multipart "$conc"
      ;;
    *)
      echo "未知 mode: $MODE (支持 single | multipart | both)"
      exit 2
      ;;
  esac
done

END_TS=$(date +%s)
ELAPSED=$((END_TS - START_TS))
echo ""
echo "════════════════════════════════════════════════════════"
echo "  全部完成，总耗时: ${ELAPSED}s"
echo "════════════════════════════════════════════════════════"
