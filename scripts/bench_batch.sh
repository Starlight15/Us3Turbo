#!/bin/bash
# bench_batch.sh — 批量压测 GDS vs RDMA 在不同并发度下的性能
# 用法: ./bench_batch.sh
set -euo pipefail

GDS_BENCH="/mnt/us3_test/xinghui.shao/gds/Us3Turbo/build/rtest/bench/gds/us3_turbo_bench_gds_put"
RDMA_BENCH="/mnt/us3_test/xinghui.shao/gds/Us3Turbo/build/rtest/bench/rdma/us3_turbo_bench_rdma_put"
PROXY="192.168.1.198:9100"
SIZE="4M"
COUNT="100"
WARMUP="10"
ROUNDS=10  # 每个并发度跑10轮

CONCURRENCIES="1 2 4 8 16 32"

echo "============================================"
echo "Batch benchmark: GDS vs RDMA, 4MB PUT"
echo "  count=$COUNT warmup=$WARMUP rounds=$ROUNDS"
echo "  concurrency: $CONCURRENCIES"
echo "  $(date)"
echo "============================================"

run_one() {
  local label="$1"   # gds/rdma
  local bin="$2"
  local conc="$3"

  local ops_sum=0.0 mibs_sum=0.0
  local ops_samples=()

  for r in $(seq 1 $ROUNDS); do
    local output ops mibs
    output=$("$bin" --proxy "$PROXY" --size "$SIZE" --count "$COUNT" --concurrency "$conc" --warmup "$WARMUP" 2>&1)
    ops=$(echo "$output" | grep -oP '\(\s*\K[0-9.]+(?=\s*ops/s\))' || echo "0")
    mibs=$(echo "$output" | grep -oP 'throughput\s*:\s*\K[0-9.]+(?=\s*MiB/s)' || echo "0")

    if [[ "$ops" == "0" || -z "$ops" ]]; then
      echo "  WARN: round $r failed to parse ops/s"
      continue
    fi
    ops_samples+=("$ops")
    ops_sum=$(awk "BEGIN {print $ops_sum + $ops}")
    mibs_sum=$(awk "BEGIN {print $mibs_sum + $mibs}")
    echo "  [$label] conc=$conc round=$r ops/s=$ops MiB/s=$mibs"
  done

  local n=${#ops_samples[@]}
  if [[ $n -eq 0 ]]; then
    echo "  [$label] conc=$conc: ALL ROUNDS FAILED"
    return
  fi

  local mean_ops mean_mibs
  mean_ops=$(awk "BEGIN {printf \"%.1f\", $ops_sum / $n}")
  mean_mibs=$(awk "BEGIN {printf \"%.1f\", $mibs_sum / $n}")

  # Calculate stddev
  local sum_sq=0.0
  for v in "${ops_samples[@]}"; do
    sum_sq=$(awk "BEGIN {d = $v - $mean_ops; print $sum_sq + d * d}")
  done
  local stddev
  stddev=$(awk "BEGIN {printf \"%.1f\", sqrt($sum_sq / $n)}")
  local cv
  cv=$(awk "BEGIN {printf \"%.1f\", ($stddev / $mean_ops) * 100}")

  echo ""
  echo "RESULT: [$label] conc=$conc mean_ops=$mean_ops mean_mibs=$mean_mibs stddev=$stddev cv_pct=$cv samples=${ops_samples[*]}"
  echo ""
}

for c in $CONCURRENCIES; do
  echo "===== concurrency=$c ====="
  run_one "GDS"  "$GDS_BENCH"  "$c"
  run_one "RDMA" "$RDMA_BENCH" "$c"
done

echo "===== DONE ====="
echo "$(date)"
