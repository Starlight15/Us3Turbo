#!/bin/bash
# run-regression.sh — 全量回归测试 (GDS + RDMA, 共 12 项)
#
# 用法:
#   bash rtest/scripts/run-regression.sh                          # 默认 proxy
#   bash rtest/scripts/run-regression.sh --proxy 10.0.0.1:9100   # 指定 proxy
#   bash rtest/scripts/run-regression.sh --gds-only               # 仅 GDS
#   bash rtest/scripts/run-regression.sh --rdma-only              # 仅 RDMA
#   bash rtest/scripts/run-regression.sh --verbose                # 打印完整输出
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
BUILD_DIR="${PROJECT_DIR}/build"

GDS_DIR="${BUILD_DIR}/rtest/regression/gds"
RDMA_DIR="${BUILD_DIR}/rtest/regression/rdma"

# ---- 默认值 ----
PROXY="192.168.1.198:9100"
MODE="all"        # all | gds | rdma
VERBOSE=false

# ---- 参数解析 ----
while [[ $# -gt 0 ]]; do
  case "$1" in
    --proxy)       PROXY="$2"; shift 2 ;;
    --gds-only)    MODE="gds"; shift ;;
    --rdma-only)   MODE="rdma"; shift ;;
    --verbose|-v)  VERBOSE=true; shift ;;
    *) echo "未知参数: $1"; exit 2 ;;
  esac
done

# ---- 编译检查 ----
RUN_GDS=false; RUN_RDMA=false
case "$MODE" in
  all)  RUN_GDS=true; RUN_RDMA=true ;;
  gds)  RUN_GDS=true ;;
  rdma) RUN_RDMA=true ;;
esac

NEED_BUILD=false
if $RUN_GDS && [ ! -x "${GDS_DIR}/us3_turbo_rtest_gds_put_single" ]; then NEED_BUILD=true; fi
if $RUN_RDMA && [ ! -x "${RDMA_DIR}/us3_turbo_rtest_rdma_put_single" ]; then NEED_BUILD=true; fi
if $NEED_BUILD; then
  echo "[build] 编译回归测试..."
  cmake --build "$BUILD_DIR" -j"$(nproc)" > /dev/null || { echo "[build] 失败"; exit 1; }
fi

# ---- 运行 ----
PASS=0; FAIL=0; TOTAL=0
declare -a FAILED_TESTS=()

run_one() {
  local name="$1"; shift
  TOTAL=$((TOTAL + 1))
  printf "[%2d/%-2d] %-55s " "$TOTAL" "$((TOTAL))" "$name"
  local log
  if log=$("$@" --proxy "$PROXY" 2>&1); then
    echo "PASS"
    PASS=$((PASS + 1))
  else
    local rc=$?
    echo "FAIL (exit=$rc)"
    FAIL=$((FAIL + 1))
    FAILED_TESTS+=("$name")
    if $VERBOSE; then echo "$log" | tail -20; fi
  fi
}

echo "══════════════════════════════════════════════════════════"
echo "  回归测试  proxy=$PROXY  mode=$MODE"
echo "══════════════════════════════════════════════════════════"
echo ""

if $RUN_GDS; then
  echo "── GDS ──"
  run_one "gds_put_single"                              "${GDS_DIR}/us3_turbo_rtest_gds_put_single"
  run_one "gds_multipart_single_part"                    "${GDS_DIR}/us3_turbo_rtest_gds_multipart_single_part"
  run_one "gds_multipart_invalid_part_size"              "${GDS_DIR}/us3_turbo_rtest_gds_multipart_invalid_part_size"
  run_one "gds_multipart_part_number_violation"          "${GDS_DIR}/us3_turbo_rtest_gds_multipart_part_number_violation"
  run_one "gds_get_single_block_crc"                     "${GDS_DIR}/us3_turbo_rtest_gds_get_single_block_crc"
  run_one "gds_get_multi_block_hash"                     "${GDS_DIR}/us3_turbo_rtest_gds_get_multi_block_hash"
  echo ""
fi

if $RUN_RDMA; then
  echo "── RDMA ──"
  run_one "rdma_put_single"                              "${RDMA_DIR}/us3_turbo_rtest_rdma_put_single"
  run_one "rdma_multipart_single_part"                    "${RDMA_DIR}/us3_turbo_rtest_rdma_multipart_single_part"
  run_one "rdma_multipart_invalid_part_size"              "${RDMA_DIR}/us3_turbo_rtest_rdma_multipart_invalid_part_size"
  run_one "rdma_multipart_part_number_violation"          "${RDMA_DIR}/us3_turbo_rtest_rdma_multipart_part_number_violation"
  run_one "rdma_get_single_block_crc"                     "${RDMA_DIR}/us3_turbo_rtest_rdma_get_single_block_crc"
  run_one "rdma_get_multi_block_hash"                     "${RDMA_DIR}/us3_turbo_rtest_rdma_get_multi_block_hash"
  echo ""
fi

# ---- 汇总 ----
echo "══════════════════════════════════════════════════════════"
echo "  total=$TOTAL  pass=$PASS  fail=$FAIL"
echo "══════════════════════════════════════════════════════════"
if [[ ${#FAILED_TESTS[@]} -gt 0 ]]; then
  echo ""
  echo "失败测试:"
  for t in "${FAILED_TESTS[@]}"; do echo "  - $t"; done
fi

exit $FAIL
