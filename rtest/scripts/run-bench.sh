#!/bin/bash
# run-bench.sh — 全量性能基准 (GDS + RDMA, PUT + GET + multipart)
#
# 用法:
#   bash rtest/scripts/run-bench.sh                              # 默认配置快速跑一轮
#   bash rtest/scripts/run-bench.sh --size 100M --count 50       # 单步 PUT 定制
#   bash rtest/scripts/run-bench.sh --conc 1,4,8,16              # 并发扫描
#   bash rtest/scripts/run-bench.sh --mode put                   # 仅单步 PUT
#   bash rtest/scripts/run-bench.sh --mode multipart             # 仅分段上传
#   bash rtest/scripts/run-bench.sh --mode get                   # 仅 GET
#   bash rtest/scripts/run-bench.sh --gds-only                   # 仅 GDS
#   bash rtest/scripts/run-bench.sh --rdma-only                  # 仅 RDMA
#   bash rtest/scripts/run-bench.sh --csv /tmp/bench.csv         # CSV 输出
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
BUILD_DIR="${PROJECT_DIR}/build"

GDS_DIR="${BUILD_DIR}/rtest/bench/gds"
RDMA_DIR="${BUILD_DIR}/rtest/bench/rdma"

# ---- 默认值 ----
PROXY="192.168.1.198:9100"
MODE="all"           # all | put | multipart | get
PATH_MODE="all"      # all | gds | rdma
SIZE="100M"
COUNT="10"
TOTAL="256M"
PART_SIZE="4M"
REPS="5"
WARMUP="1"
CONCURRENCIES="1"
VERIFY=""
CSV_FILE=""

# ---- 参数解析 ----
while [[ $# -gt 0 ]]; do
  case "$1" in
    --proxy)         PROXY="$2"; shift 2 ;;
    --mode)          MODE="$2"; shift 2 ;;
    --size)          SIZE="$2"; shift 2 ;;
    --count)         COUNT="$2"; shift 2 ;;
    --total)         TOTAL="$2"; shift 2 ;;
    --part-size)     PART_SIZE="$2"; shift 2 ;;
    --reps)          REPS="$2"; shift 2 ;;
    --warmup)        WARMUP="$2"; shift 2 ;;
    --conc)          CONCURRENCIES="$2"; shift 2 ;;
    --verify)        VERIFY="--verify-crc32c"; shift ;;
    --csv)           CSV_FILE="$2"; shift 2 ;;
    --gds-only)      PATH_MODE="gds"; shift ;;
    --rdma-only)     PATH_MODE="rdma"; shift ;;
    *) echo "未知参数: $1"; exit 2 ;;
  esac
done

# ---- 编译检查 ----
NEED_BUILD=false
check_bin() { [[ -x "$1" ]] || NEED_BUILD=true; }
if [[ "$PATH_MODE" == "all" || "$PATH_MODE" == "gds" ]]; then
  check_bin "${GDS_DIR}/us3_turbo_bench_gds_put"
  check_bin "${GDS_DIR}/us3_turbo_bench_gds_multipart"
fi
if [[ "$PATH_MODE" == "all" || "$PATH_MODE" == "rdma" ]]; then
  check_bin "${RDMA_DIR}/us3_turbo_bench_rdma_put"
  check_bin "${RDMA_DIR}/us3_turbo_bench_rdma_get"
  check_bin "${RDMA_DIR}/us3_turbo_bench_rdma_multipart"
fi
if $NEED_BUILD; then
  echo "[build] 编译 bench..."
  cmake --build "$BUILD_DIR" -j"$(nproc)" > /dev/null || { echo "[build] 失败"; exit 1; }
fi

IFS=',' read -ra CONC_LIST <<< "$CONCURRENCIES"

# ---- 工具函数 ----
divider() { echo "──────────────────────────────────────────────────────────"; }

run_put() {
  local label="$1" bin="$2"; shift 2
  for conc in "${CONC_LIST[@]}"; do
    divider
    echo "[$label] PUT  size=$SIZE  count=$COUNT  conc=$conc"
    divider
    "$bin" --proxy "$PROXY" --size "$SIZE" --count "$COUNT" \
      --concurrency "$conc" --warmup "$WARMUP" $VERIFY
    echo ""
  done
}

run_multipart() {
  local label="$1" bin="$2"; shift 2
  for conc in "${CONC_LIST[@]}"; do
    divider
    echo "[$label] multipart  total=$TOTAL  part=$PART_SIZE  reps=$REPS  conc=$conc"
    divider
    local csv_arg=""
    [[ -n "$CSV_FILE" ]] && csv_arg="--csv"
    "$bin" --proxy "$PROXY" --total "$TOTAL" --part-size "$PART_SIZE" \
      --reps "$REPS" --warmup "$WARMUP" --concurrency "$conc" $VERIFY $csv_arg
    echo ""
  done
}

run_get() {
  local label="$1" bin="$2"; shift 2
  for conc in "${CONC_LIST[@]}"; do
    divider
    echo "[$label] GET  size=$SIZE  count=$COUNT  conc=$conc"
    divider
    "$bin" --proxy "$PROXY" --size "$SIZE" --count "$COUNT" \
      --concurrency "$conc" --warmup "$WARMUP"
    echo ""
  done
}

# ---- 主流程 ----
echo "══════════════════════════════════════════════════════════"
echo "  Bench  proxy=$PROXY  mode=$MODE  path=$PATH_MODE"
echo "  conc=$CONCURRENCIES  warmup=$WARMUP"
if [[ -n "$CSV_FILE" ]]; then echo "  csv=$CSV_FILE"; fi
echo "══════════════════════════════════════════════════════════"
echo ""

RUN_PUT=false; RUN_MP=false; RUN_GET=false
case "$MODE" in
  all)        RUN_PUT=true; RUN_MP=true; RUN_GET=true ;;
  put)        RUN_PUT=true ;;
  multipart)  RUN_MP=true ;;
  get)        RUN_GET=true ;;
  *) echo "未知 mode: $MODE (支持 all | put | multipart | get)"; exit 2 ;;
esac

if $RUN_PUT; then
  if [[ "$PATH_MODE" == "all" || "$PATH_MODE" == "gds" ]]; then
    run_put "GDS" "${GDS_DIR}/us3_turbo_bench_gds_put"
  fi
  if [[ "$PATH_MODE" == "all" || "$PATH_MODE" == "rdma" ]]; then
    run_put "RDMA" "${RDMA_DIR}/us3_turbo_bench_rdma_put"
  fi
fi

if $RUN_MP; then
  if [[ "$PATH_MODE" == "all" || "$PATH_MODE" == "gds" ]]; then
    run_multipart "GDS" "${GDS_DIR}/us3_turbo_bench_gds_multipart"
  fi
  if [[ "$PATH_MODE" == "all" || "$PATH_MODE" == "rdma" ]]; then
    run_multipart "RDMA" "${RDMA_DIR}/us3_turbo_bench_rdma_multipart"
  fi
fi

if $RUN_GET; then
  if [[ "$PATH_MODE" == "all" || "$PATH_MODE" == "rdma" ]]; then
    run_get "RDMA" "${RDMA_DIR}/us3_turbo_bench_rdma_get"
  fi
fi

echo "══════════════════════════════════════════════════════════"
echo "  完成"
echo "══════════════════════════════════════════════════════════"
