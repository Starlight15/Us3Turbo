#!/usr/bin/env bash
# test_rdma_put.sh — 验证 RDMA(UCX) PUT 链路端到端。
#
# 链路：client(host 内存) → proxy → backend(ucp_get_nbx 反向拉)。
# 与 GDS 链路独立。backend/proxy 二进制同时承载 gds+rdma 两条链路
# （同一进程，同一 brpc service）。
#
# 不用 pkill -f（会误杀启动 shell）；按 PID kill。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)/build"
LOG_DIR="${LOG_DIR:-$(cd "${SCRIPT_DIR}/.." && pwd)/test/log}"
mkdir -p "${LOG_DIR}"

PROXY_HOST="192.168.1.198"
PROXY_PORT="9100"
BACKEND_HOST="192.168.1.198"
BACKEND_PORT="9200"
RDMA_PORT="18516"   # cuObjServer GDS 端口（rdma 链路不用它，但 backend 启动需要）

PROXY_EP="${PROXY_HOST}:${PROXY_PORT}"
BACKEND_EP="${BACKEND_HOST}:${BACKEND_PORT}"

BACKEND_BIN="${BUILD_DIR}/backend/us3_turbo_backend"
PROXY_BIN="${BUILD_DIR}/proxy/us3_turbo_proxy"
CLIENT_BIN="${BUILD_DIR}/examples/us3_turbo_rdma_put_example"

for bin in "$BACKEND_BIN" "$PROXY_BIN" "$CLIENT_BIN"; do
  if [[ ! -x "$bin" ]]; then
    echo "[ERROR] not found or not executable: $bin"
    echo "        run: cd ${BUILD_DIR} && make -j$(nproc)"
    exit 1
  fi
done

BACKEND_PID=""
PROXY_PID=""
cleanup() {
  echo ""
  echo "[cleanup] stopping backend/proxy..."
  [[ -n "$PROXY_PID" ]]  && kill "$PROXY_PID"  2>/dev/null || true
  [[ -n "$BACKEND_PID" ]] && kill "$BACKEND_PID" 2>/dev/null || true
  wait 2>/dev/null || true
  echo "[cleanup] done"
}
trap cleanup EXIT INT TERM

echo "[start] backend on ${BACKEND_EP}"
"$BACKEND_BIN" \
  --bind_host="$BACKEND_HOST" \
  --backend_brpc_port="$BACKEND_PORT" \
  --backend_rdma_port="$RDMA_PORT" \
  --backend_id=backend-0 \
  &> "${LOG_DIR}/backend_rdma_test.log" &
BACKEND_PID=$!
echo "        backend pid=$BACKEND_PID"

echo "[start] proxy on ${PROXY_EP}"
"$PROXY_BIN" \
  --bind_host="$PROXY_HOST" \
  --proxy_port="$PROXY_PORT" \
  --backend_endpoint="$BACKEND_EP" \
  &> "${LOG_DIR}/proxy_rdma_test.log" &
PROXY_PID=$!
echo "        proxy pid=$PROXY_PID"

echo "[wait] waiting for services to start..."
sleep 3

for pid_var in BACKEND_PID PROXY_PID; do
  pid="${!pid_var}"
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "[ERROR] ${pid_var%_PID} (pid=$pid) died at startup"
    echo "=== backend_rdma_test.log ==="; cat "${LOG_DIR}/backend_rdma_test.log"
    echo "=== proxy_rdma_test.log ===";  cat "${LOG_DIR}/proxy_rdma_test.log"
    exit 1
  fi
done
echo "[ok] backend and proxy are running"

echo ""
echo "============================================================"
echo "  Testing RDMA(UCX) PUT"
echo "  Proxy:   ${PROXY_EP}"
echo "  Backend: ${BACKEND_EP}"
echo "============================================================"
echo ""

SIZE="${SIZE:-100M}"
"$CLIENT_BIN" --proxy "$PROXY_EP" --size "$SIZE" --verify-crc32c
RC=$?

echo ""
if [[ $RC -eq 0 ]]; then
  echo "✅ [SUCCESS] RDMA PUT test passed"
else
  echo "❌ [FAILED] RDMA PUT test failed (exit=$RC)"
fi

echo ""
echo "=== backend_rdma_test.log (last 30 lines) ==="
tail -30 "${LOG_DIR}/backend_rdma_test.log"
echo ""
echo "=== proxy_rdma_test.log (last 30 lines) ==="
tail -30 "${LOG_DIR}/proxy_rdma_test.log"

exit $RC
