# GDS regression tests — 每个独立可执行，命名 us3_turbo_rtest_gds_<name>

# T1.1 — 中间 part < 16MB 应在 Complete 时被拒绝
rtest_add_target(us3_turbo_rtest_gds_multipart_invalid_part_size
  test_multipart_invalid_part_size.cpp NEEDS_CUDA)

# T1.2 — part_number 重复 / 跳号
rtest_add_target(us3_turbo_rtest_gds_multipart_part_number_violation
  test_multipart_part_number_violation.cpp NEEDS_CUDA)

# T1.3 — 单 part = 整对象
rtest_add_target(us3_turbo_rtest_gds_multipart_single_part
  test_multipart_single_part.cpp NEEDS_CUDA)

# T2.1 — GET 单块对象 crc32c
rtest_add_target(us3_turbo_rtest_gds_get_single_block_crc
  test_get_single_block_crc.cpp NEEDS_CUDA)

# T2.2 — GET 多块对象 hash
rtest_add_target(us3_turbo_rtest_gds_get_multi_block_hash
  test_get_multi_block_hash.cpp NEEDS_CUDA)
