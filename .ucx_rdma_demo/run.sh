#!/usr/bin/env bash
# run.sh — 起 source(client) + puller(backend) 跑 UCX RDMA demo。
#
# 默认绑 192.168.1.198（mlx5 fabric，走 rc_mlx5）。一次验证同时证明
# API 调用链 + RDMA fabric 可达。
#
# 不用 pkill -f（会误杀启动 shell），按 PID kill。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

HOST="${HOST:-192.168.1.198}"
PORT="${PORT:-39321}"
BYTES="${BYTES:-1048576}"

SOURCE_BIN="${SCRIPT_DIR}/source"
PULLER_BIN="${SCRIPT_DIR}/puller"

for b in "$SOURCE_BIN" "$PULLER_BIN"; do
  if [[ ! -x "$b" ]]; then
    echo "[ERROR] not found: $b (run: make -C $SCRIPT_DIR)" >&2
    exit 1
  fi
done

SRC_PID=""
cleanup() {
  if [[ -n "$SRC_PID" ]] && kill -0 "$SRC_PID" 2>/dev/null; then
    kill "$SRC_PID" 2>/dev/null || true
    wait "$SRC_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

echo "[start] source on ${HOST}:${PORT} (bytes=${BYTES})"
"$SOURCE_BIN" "$HOST" "$PORT" "$BYTES" &
SRC_PID=$!
sleep 1

if ! kill -0 "$SRC_PID" 2>/dev/null; then
  echo "[ERROR] source died at startup" >&2
  exit 1
fi

echo "[start] puller -> ${HOST}:${PORT}"
echo "============================================================"
"$PULLER_BIN" "$HOST" "$PORT" "$BYTES"
RC=$?
echo "============================================================"
echo "puller exit=$RC"

exit $RC
