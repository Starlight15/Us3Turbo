# RDMA (libibverbs) regression tests — host 内存，无 CUDA 依赖

# T1.1 — 中间 part < 16MB 应在 Complete 时被拒绝
rtest_add_target(us3_turbo_rtest_rdma_multipart_invalid_part_size
  test_multipart_invalid_part_size.cpp)

# T1.2 — part_number 重复 / 跳号
rtest_add_target(us3_turbo_rtest_rdma_multipart_part_number_violation
  test_multipart_part_number_violation.cpp)

# T1.3 — 单 part = 整对象
rtest_add_target(us3_turbo_rtest_rdma_multipart_single_part
  test_multipart_single_part.cpp)

# T3.1 — 单步 PUT（1K / 4M / 16M 尺寸验证）
rtest_add_target(us3_turbo_rtest_rdma_put_single
  test_put_single.cpp)

# T2.1 — GET 单块对象 CRC 一致性
rtest_add_target(us3_turbo_rtest_rdma_get_single_block_crc
  test_get_single_block_crc.cpp)

# T2.2 — GET 多块对象 hash 验证
rtest_add_target(us3_turbo_rtest_rdma_get_multi_block_hash
  test_get_multi_block_hash.cpp)
