#!/bin/bash
# bench_collect.sh — 逐个执行测试，带超时，收集结果到 result CSV
set -u

BENCH="/mnt/us3_test/xinghui.shao/gds/Us3Turbo/scripts/bench_put_4mb.sh"
OUT="/mnt/us3_test/xinghui.shao/gds/Us3Turbo/scripts/bench_full_results.txt"

# 测试矩阵
SIZES="4M 8M"
CONCS="1 2 4 8 16 24 32"
MOCK_NAMES="no-disk:1 w-disk:0"
PATHS="gds rdma"

echo "path,size,conc,mode,ops_s,mibs,rounds,cv_pct" > "$OUT.csv"

run_one() {
  local path="$1" size="$2" conc="$3" mock="$4" mode="$5"
  local maxr=20
  echo -n "[$(date +%H:%M:%S)] $path $size c=$conc $mode ... "

  local output
  if output=$(timeout 180 bash "$BENCH" \
      --path "$path" --concurrency "$conc" --mock-aio-write "$mock" \
      --size "$size" --max-rounds "$maxr" 2>&1); then
    :
  else
    local rc=$?
    if [ $rc -eq 124 ]; then
      echo "TIMEOUT"
      fuser -k 9100/tcp 2>/dev/null; fuser -k 24000/tcp 2>/dev/null
      return
    fi
  fi

  local result
  result=$(echo "$output" | grep 'RESULT_JSON:' | tail -1)
  if [ -n "$result" ]; then
    # Parse JSON-like line
    local ops rounds cv
    ops=$(echo "$result" | grep -oP '"ops_per_sec":\K[0-9.]+')
    rounds=$(echo "$result" | grep -oP '"rounds":\K[0-9]+')
    cv=$(echo "$result" | grep -oP '"max_dev_pct":\K[0-9.]+')
    local mibs=$(awk "BEGIN {printf \"%.0f\", $ops * ${size%M} / 1024}")
    echo "$path,$size,$conc,$mode,$ops,$mibs,$rounds,$cv" >> "$OUT.csv"
    echo "OK ops=$ops MiB/s=$mibs cv=$cv%"
  else
    # Try best-effort
    local ops
    ops=$(echo "$output" | grep -oP 'best-effort mean ops/s = \K[0-9]+' || echo "")
    if [ -n "$ops" ]; then
      echo "$path,$size,$conc,$mode,$ops,-,-,-" >> "$OUT.csv"
      echo "FALLBACK ops=$ops"
    else
      echo "FAIL"
      fuser -k 9100/tcp 2>/dev/null; fuser -k 24000/tcp 2>/dev/null
    fi
  fi
}

# Main
echo "Starting matrix: $(date)"
echo "path,size,conc,mode,ops_s,mibs,rounds,cv_pct" > "$OUT.csv"

for MOCK_NAME in $MOCK_NAMES; do
  MODE="${MOCK_NAME%:*}"
  MOCK="${MOCK_NAME#*:}"
  for SIZE in $SIZES; do
    for CONC in $CONCS; do
      for PATH_NAME in $PATHS; do
        run_one "$PATH_NAME" "$SIZE" "$CONC" "$MOCK" "$MODE"
        sleep 1  # brief pause between tests
      done
    done
  done
done

echo "DONE: $(date)"
echo ""
echo "=== CSV Results ==="
cat "$OUT.csv"
