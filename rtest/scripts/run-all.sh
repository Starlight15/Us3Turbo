#!/bin/bash
# rtest 全量回归运行脚本（GDS + RDMA）
# 环境前置: mongod(27017) → dbgate(20165) → ufile-ac(24000) → proxy(9100) 全部单进程运行
# 关键: proxy 空闲数分钟后 dbgate 连接会 CLOSE-WAIT 死亡(见 TEST_FINDINGS.md P1)，
#       跑 multipart 测试前若 proxy 已空闲建议先重启 proxy。
# 用法: bash rtest/scripts/run-all.sh
set -u
cd "$(dirname "$0")/../.." || exit 1  # cd to repo root (run-all.sh is in rtest/scripts/)
BUILD=build
PROXY="${PROXY:-192.168.1.198:9100}"

run() { echo "########## $1 ##########"; timeout 200 "$1" --proxy "$PROXY" "${@:2}"; echo "EXIT=$?"; echo; }

GDS=$BUILD/rtest/regression/gds
RDMA=$BUILD/rtest/regression/rdma

echo "===== GDS ====="
run "$GDS/us3_turbo_rtest_gds_multipart_invalid_part_size"      --part-size 3M
run "$GDS/us3_turbo_rtest_gds_multipart_part_number_violation"  --part-size 4M
run "$GDS/us3_turbo_rtest_gds_multipart_single_part"            --part-size 4M
run "$GDS/us3_turbo_rtest_gds_get_single_block_crc"             --size 2M
run "$GDS/us3_turbo_rtest_gds_get_multi_block_hash"             --size 8M

echo "===== RDMA ====="
run "$RDMA/us3_turbo_rtest_rdma_multipart_invalid_part_size"      --part-size 3M
run "$RDMA/us3_turbo_rtest_rdma_multipart_part_number_violation"  --part-size 4M
run "$RDMA/us3_turbo_rtest_rdma_multipart_single_part"            --part-size 4M
run "$RDMA/us3_turbo_rtest_rdma_get_single_block_crc"             --size 2M
run "$RDMA/us3_turbo_rtest_rdma_get_multi_block_hash"             --size 4M
