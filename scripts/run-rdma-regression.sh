#!/bin/bash
# run-rdma-regression.sh — RDMA (libibverbs) 回归测试一键运行
#
# 用法:
#   ./scripts/run-rdma-regression.sh                           # 默认 proxy + 4M part-size
#   ./scripts/run-rdma-regression.sh --proxy 10.0.0.1:9100    # 指定 proxy
#   ./scripts/run-rdma-regression.sh --part-size 8M            # 指定 part 大小
#   ./scripts/run-rdma-regression.sh --verify-crc32c            # 开启 CRC32C 校验
#
# 依赖: 编译产物须已存在于 build/rtest/regression/rdma/ 下。
#       若不存在，脚本自动调用 cmake --build 编译。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
BIN_DIR="${BUILD_DIR}/rtest/regression/rdma"

# ---- 默认参数 ----
PROXY="192.168.1.198:9100"
PART_SIZE="4M"
VERIFY=""

# ---- 参数解析 ----
while [[ $# -gt 0 ]]; do
  case "$1" in
    --proxy)
      PROXY="$2"; shift 2 ;;
    --part-size)
      PART_SIZE="$2"; shift 2 ;;
    --verify-crc32c)
      VERIFY="--verify-crc32c"; shift ;;
    --help|-h)
      echo "用法: $0 [选项]"
      echo "  --proxy ADDR        proxy 地址 (默认 192.168.1.198:9100)"
      echo "  --part-size SIZE    part 大小 (默认 4M)"
      echo "  --verify-crc32c     开启端到端 CRC32C 校验 (T1.3)"
      exit 0 ;;
    *)
      echo "未知参数: $1"; exit 2 ;;
  esac
done

# ---- 编译检查 ----
BINS=(
  "$BIN_DIR/us3_turbo_rtest_rdma_multipart_invalid_part_size"
  "$BIN_DIR/us3_turbo_rtest_rdma_multipart_part_number_violation"
  "$BIN_DIR/us3_turbo_rtest_rdma_multipart_single_part"
)

NEED_BUILD=false
for bin in "${BINS[@]}"; do
  if [[ ! -x "$bin" ]]; then
    NEED_BUILD=true
    break
  fi
done

if $NEED_BUILD; then
  echo "[build] 编译 RDMA 回归测试..."
  cmake --build "$BUILD_DIR" -j"$(nproc)" \
    --target us3_turbo_rtest_rdma_multipart_invalid_part_size \
    --target us3_turbo_rtest_rdma_multipart_part_number_violation \
    --target us3_turbo_rtest_rdma_multipart_single_part \
    || { echo "[build] 编译失败"; exit 1; }
fi

# ---- 运行测试 ----
PASS=0
FAIL=0
SKIP=0
TOTAL=0

run_test() {
  local name="$1"
  shift
  echo ""
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  echo "[test] $name"
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  TOTAL=$((TOTAL + 1))
  if "$@"; then
    PASS=$((PASS + 1))
    echo "[result] $name: PASS"
  else
    local rc=$?
    if [[ $rc -eq 77 ]]; then
      SKIP=$((SKIP + 1))
      echo "[result] $name: SKIP (环境不稳定)"
    else
      FAIL=$((FAIL + 1))
      echo "[result] $name: FAIL (exit=$rc)"
    fi
  fi
}

# T1.1 — 中间 part < part_size 应在 Complete 时被拒绝
run_test "T1.1 invalid_part_size" \
  "${BINS[0]}" --proxy "$PROXY" --part-size "$PART_SIZE"

# T1.2 — part_number 重复 / 跳号
run_test "T1.2 part_number_violation" \
  "${BINS[1]}" --proxy "$PROXY"

# T1.3 — 单 part = 整对象
VERIFY_ARG=""
[[ -n "$VERIFY" ]] && VERIFY_ARG="--verify-crc32c"
run_test "T1.3 single_part" \
  "${BINS[2]}" --proxy "$PROXY" ${VERIFY_ARG:+"$VERIFY_ARG"}

# ---- 汇总 ----
echo ""
echo "════════════════════════════════════════════════════════"
echo "  total : $TOTAL"
echo "  pass  : $PASS"
echo "  fail  : $FAIL"
echo "  skip  : $SKIP"
echo "════════════════════════════════════════════════════════"

if [[ $FAIL -gt 0 ]]; then
  exit 1
fi
exit 0
