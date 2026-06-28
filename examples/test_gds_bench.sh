#!/usr/bin/env bash
# test_gds_bench.sh — 拉起 backend + proxy 后跑 GDS PUT 基准测试。
#
# 功能:
#   1. 强制 kill 旧的 backend/proxy 进程
#   2. 在 192.168.1.198 上启动 backend + proxy
#   3. 运行 gds_bench_example(参数透传:对象大小 / 数量 / 并发 等)
#   4. 测试完成后自动 kill 所有进程
#
# 用法:
#   bash examples/test_gds_bench.sh [bench options...]
#     --size 100M --count 100 --concurrency 8
#   默认: --size 100M --count 10 --concurrency 1

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)/build"

# 默认日志目录(仓库内 test/log,可被环境变量覆盖)。
LOG_DIR="${LOG_DIR:-$(cd "${SCRIPT_DIR}/.." && pwd)/test/log}"
mkdir -p "${LOG_DIR}"

# ============================================================
#  端口配置(与 gds_put_example.cpp 硬编码的 endpoint 对应)
# ============================================================

PROXY_HOST="192.168.1.198"
PROXY_PORT="9100"
BACKEND_HOST="192.168.1.198"
BACKEND_PORT="9200"
RDMA_PORT="18516"

PROXY_EP="${PROXY_HOST}:${PROXY_PORT}"
BACKEND_EP="${BACKEND_HOST}:${BACKEND_PORT}"

# ============================================================
#  二进制路径
# ============================================================

BACKEND_BIN="${BUILD_DIR}/backend/us3_turbo_backend"
PROXY_BIN="${BUILD_DIR}/proxy/us3_turbo_proxy"
BENCH_BIN="${BUILD_DIR}/examples/us3_turbo_gds_bench_example"

# ---- 检查二进制存在 ----
for bin in "$BACKEND_BIN" "$PROXY_BIN" "$BENCH_BIN"; do
  if [[ ! -x "$bin" ]]; then
    echo "[ERROR] not found or not executable: $bin"
    echo "        run: cd ${SCRIPT_DIR}/.. && bash do_make.sh"
    exit 1
  fi
done

# ============================================================
#  强制清理旧进程(避免端口占用)
# ============================================================

echo "[cleanup] killing old backend/proxy processes..."
pkill -9 -f us3_turbo_backend || true
pkill -9 -f us3_turbo_proxy || true
sleep 1

# ============================================================
#  启动新进程
# ============================================================

BACKEND_PID=""
PROXY_PID=""

cleanup() {
  echo ""
  echo "[cleanup] stopping backend/proxy..."
  [[ -n "$BACKEND_PID" ]] && kill -9 "$BACKEND_PID" 2>/dev/null || true
  [[ -n "$PROXY_PID" ]] && kill -9 "$PROXY_PID" 2>/dev/null || true
  wait 2>/dev/null || true
  echo "[cleanup] done"
}
trap cleanup EXIT INT TERM

# ---- 启动 Backend ----
echo "[start] backend on ${BACKEND_EP} (RDMA port ${RDMA_PORT})"
"$BACKEND_BIN" \
  --bind_host="$BACKEND_HOST" \
  --backend_brpc_port="$BACKEND_PORT" \
  --backend_rdma_port="$RDMA_PORT" \
  --backend_id=backend-0 \
  &> "${LOG_DIR}/backend_bench.log" &
BACKEND_PID=$!
echo "        backend pid=$BACKEND_PID, logs: ${LOG_DIR}/backend_bench.log"

# ---- 启动 Proxy ----
echo "[start] proxy on ${PROXY_EP}"
"$PROXY_BIN" \
  --bind_host="$PROXY_HOST" \
  --proxy_port="$PROXY_PORT" \
  --backend_endpoint="$BACKEND_EP" \
  &> "${LOG_DIR}/proxy_bench.log" &
PROXY_PID=$!
echo "        proxy pid=$PROXY_PID, logs: ${LOG_DIR}/proxy_bench.log"

# 等待服务启动
echo "[wait] waiting for services to start..."
sleep 3

# ---- 验证进程存活 ----
for pid_var in BACKEND_PID PROXY_PID; do
  pid="${!pid_var}"
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "[ERROR] ${pid_var%_PID} (pid=$pid) died at startup"
    echo ""
    echo "=== backend_bench.log ==="
    cat "${LOG_DIR}/backend_bench.log"
    echo ""
    echo "=== proxy_bench.log ==="
    cat "${LOG_DIR}/proxy_bench.log"
    exit 1
  fi
done

echo "[ok] backend and proxy are running"

# ============================================================
#  运行 GDS PUT Bench
# ============================================================

# 若未传任何 bench 参数,使用默认配置。
BENCH_ARGS=("$@")
if [[ ${#BENCH_ARGS[@]} -eq 0 ]]; then
  BENCH_ARGS=(--size 100M --count 10 --concurrency 1)
fi

echo ""
echo "============================================================"
echo "  GDS PUT bench"
echo "  Proxy:   ${PROXY_EP}"
echo "  Backend: ${BACKEND_EP}"
echo "  Args:    ${BENCH_ARGS[*]}"
echo "============================================================"
echo ""

"$BENCH_BIN" \
  --proxy "$PROXY_EP" \
  "${BENCH_ARGS[@]}" 2>&1 | tee "${LOG_DIR}/bench.log"
RC=${PIPESTATUS[0]}

echo ""
if [[ $RC -eq 0 ]]; then
  echo "✅ [SUCCESS] GDS bench finished (all ops ok)"
elif [[ $RC -eq 2 ]]; then
  echo "⚠️  [PARTIAL] GDS bench finished with failures (see results above)"
else
  echo "❌ [FAILED] GDS bench failed (exit=$RC)"
fi

# ============================================================
#  打印服务端日志尾部
# ============================================================

echo ""
echo "=== backend_bench.log (last 30 lines) ==="
tail -30 "${LOG_DIR}/backend_bench.log"
echo ""
echo "=== proxy_bench.log (last 30 lines) ==="
tail -30 "${LOG_DIR}/proxy_bench.log"

exit $RC
