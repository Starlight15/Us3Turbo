#!/usr/bin/env bash
# test_gds_pimpl.sh — 验证 ClientCore PImpl 重构后的 GDS 端到端功能
#
# 功能：
#   1. 强制 kill 旧的 backend/proxy 进程
#   2. 在 192.168.1.198 上启动 backend + proxy
#   3. 运行 gds_put_example（硬编码连接到 192.168.1.198）
#   4. 测试完成后自动 kill 所有进程

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)/build"

# 日志目录（/tmp 可能满，改用 /var/log）
LOG_DIR="/var/log"

# ============================================================
#  端口配置（与 gds_put_example.cpp 硬编码的 endpoint 对应）
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
CLIENT_BIN="${BUILD_DIR}/examples/us3_turbo_gds_put_example"

# ---- 检查二进制存在 ----
for bin in "$BACKEND_BIN" "$PROXY_BIN" "$CLIENT_BIN"; do
  if [[ ! -x "$bin" ]]; then
    echo "[ERROR] not found or not executable: $bin"
    echo "        run: cd $BUILD_DIR && make -j$(nproc)"
    exit 1
  fi
done

# ============================================================
#  强制清理旧进程（避免端口占用）
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

# 清理函数（测试完成或 Ctrl+C 时杀进程）
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
  --proxy_endpoint="$PROXY_EP" \
  &> ${LOG_DIR}/backend_gds_test.log &
BACKEND_PID=$!
echo "        backend pid=$BACKEND_PID, logs: ${LOG_DIR}/backend_gds_test.log"

# ---- 启动 Proxy ----
echo "[start] proxy on ${PROXY_EP}"
"$PROXY_BIN" \
  --bind_host="$PROXY_HOST" \
  --proxy_port="$PROXY_PORT" \
  --backend_endpoint="$BACKEND_EP" \
  &> ${LOG_DIR}/proxy_gds_test.log &
PROXY_PID=$!
echo "        proxy pid=$PROXY_PID, logs: ${LOG_DIR}/proxy_gds_test.log"

# 等待服务启动
echo "[wait] waiting for services to start..."
sleep 3

# ---- 验证进程存活 ----
for pid_var in BACKEND_PID PROXY_PID; do
  pid="${!pid_var}"
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "[ERROR] ${pid_var%_PID} (pid=$pid) died at startup"
    echo ""
    echo "=== backend_gds_test.log ==="
    cat ${LOG_DIR}/backend_gds_test.log
    echo ""
    echo "=== proxy_gds_test.log ==="
    cat ${LOG_DIR}/proxy_gds_test.log
    exit 1
  fi
done

echo "[ok] backend and proxy are running"

# ============================================================
#  运行 GDS PUT Example（验证 PImpl 重构后的端到端功能）
# ============================================================

echo ""
echo "============================================================"
echo "  Testing GDS PUT (ClientCore PImpl verification)"
echo "  Proxy:   ${PROXY_EP}"
echo "  Backend: ${BACKEND_EP}"
echo "  Client:  gds_put_example (hardcoded params)"
echo "============================================================"
echo ""

"$CLIENT_BIN"
RC=$?

echo ""
if [[ $RC -eq 0 ]]; then
  echo "✅ [SUCCESS] GDS client test passed - PImpl refactoring verified"
else
  echo "❌ [FAILED] GDS client test failed (exit=$RC)"
fi

# ============================================================
#  打印服务端日志
# ============================================================

echo ""
echo "=== backend_gds_test.log (last 50 lines) ==="
tail -50 ${LOG_DIR}/backend_gds_test.log
echo ""
echo "=== proxy_gds_test.log (last 50 lines) ==="
tail -50 ${LOG_DIR}/proxy_gds_test.log

exit $RC
