#!/bin/bash
# run_bench.sh — 性能基准测试脚本 (GDS + RDMA, PUT + GET + multipart)
#
# 仅控制 client 端参数，不管理 ufile-ac / proxy 服务端。
# 运行前需确保:
#   - ufile-ac 已启动 (端口 24000)
#   - proxy 已启动 (端口 9100)
#   - 对应的 bench 二进制已编译 (脚本会自动检查并触发 cmake --build)
#
# 用法:
#   # 默认配置快速跑一轮 (全量: GDS+RDMA, PUT+multipart+GET)
#   bash rtest/scripts/run_bench.sh
#
#   # 仅单步 PUT
#   bash rtest/scripts/run_bench.sh --mode put
#
#   # 仅分段上传 (multipart)
#   bash rtest/scripts/run_bench.sh --mode multipart
#
#   # 仅 GET
#   bash rtest/scripts/run_bench.sh --mode get
#
#   # 指定数据通路
#   bash rtest/scripts/run_bench.sh --path gds
#   bash rtest/scripts/run_bench.sh --path rdma
#
#   # 定制对象大小和数量
#   bash rtest/scripts/run_bench.sh --size 64M --count 100
#
#   # 定制分段上传大小和分片
#   bash rtest/scripts/run_bench.sh --mode multipart --total 256M --part-size 4M --reps 10
#
#   # 并发扫描 (逗号分隔，无空格)
#   bash rtest/scripts/run_bench.sh --conc 1,4,8,16,32
#
#   # 启用 CRC32C 校验
#   bash rtest/scripts/run_bench.sh --verify
#
#   # 输出 CSV (multipart 模式)
#   bash rtest/scripts/run_bench.sh --mode multipart --csv /tmp/bench.csv
#
#   # 指定 proxy
#   bash rtest/scripts/run_bench.sh --proxy 10.0.0.1:9100
#
#   # 组合示例
#   bash rtest/scripts/run_bench.sh --path rdma --mode put --size 4M --count 200 --conc 1,8,16
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
    --path)          PATH_MODE="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,46p' "${BASH_SOURCE[0]}"
      exit 0 ;;
    *) echo "未知参数: $1"; exit 2 ;;
  esac
done

# 校验 --path
if [[ "$PATH_MODE" != "all" && "$PATH_MODE" != "gds" && "$PATH_MODE" != "rdma" ]]; then
  echo "ERROR: --path 必须是 gds, rdma 或 all, 当前: $PATH_MODE"
  exit 2
fi

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
[[ -n "$CSV_FILE" ]] && echo "  csv=$CSV_FILE"
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
  if [[ "$PATH_MODE" == "all" || "$PATH_MODE" == "gds" ]]; then
    echo "── GDS GET 无 bench（暂未实现），跳过 ──"
  fi
fi

echo "══════════════════════════════════════════════════════════"
echo "  完成"
echo "══════════════════════════════════════════════════════════"
