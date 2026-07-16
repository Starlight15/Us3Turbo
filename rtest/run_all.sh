#!/bin/bash
# rtest 运行脚本（GDS + UCX）
# 环境前置: mongod(27017) → dbgate(20165) → ufile-ac(24000) → proxy(9100) 全部单进程运行
# 关键: proxy 空闲数分钟后 dbgate 连接会 CLOSE-WAIT 死亡(见 TEST_FINDINGS.md P1)，
#       跑 multipart 测试前若 proxy 已空闲建议先重启 proxy。
# UCX 关键: client 需 UCX_NET_DEVICES=mlx5_2:1(与 ufile-ac net_devices 同网段)，
#          否则默认选 mlx5_0(跨网段)建连超时(见 TEST_FINDINGS.md P2)。
set -u
cd "$(dirname "$0")/.." || exit 1
BUILD=build
PROXY="${PROXY:-192.168.1.198:9100}"
export UCX_NET_DEVICES="${UCX_NET_DEVICES:-mlx5_2:1}"

run() { echo "########## $1 ##########"; timeout 200 "$1" --proxy "$PROXY" "${@:2}"; echo "EXIT=$?"; echo; }

GDS=$BUILD/rtest/regression/gds
UCX=$BUILD/rtest/regression/ucx

echo "===== GDS ====="
run "$GDS/us3_turbo_rtest_gds_multipart_invalid_part_size"      --part-size 8M
run "$GDS/us3_turbo_rtest_gds_multipart_part_number_violation"  --part-size 16M
run "$GDS/us3_turbo_rtest_gds_multipart_single_part"            --part-size 16M
run "$GDS/us3_turbo_rtest_gds_get_single_block_crc"             --size 2M
run "$GDS/us3_turbo_rtest_gds_get_multi_block_hash"             --size 20M

echo "===== UCX ====="
run "$UCX/us3_turbo_rtest_ucx_multipart_invalid_part_size"      --part-size 8M
run "$UCX/us3_turbo_rtest_ucx_multipart_part_number_violation"  --part-size 16M
run "$UCX/us3_turbo_rtest_ucx_multipart_single_part"            --part-size 16M
run "$UCX/us3_turbo_rtest_ucx_get_single_block_crc"             --size 2M
run "$UCX/us3_turbo_rtest_ucx_get_multi_block_hash"             --size 20M
