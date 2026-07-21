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
