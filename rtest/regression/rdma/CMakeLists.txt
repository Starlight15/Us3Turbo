# RDMA (libibverbs) regression tests — host 内存，无 CUDA 依赖
set(RTEST_PRIVATE_INCLUDES
  ${PROJECT_SOURCE_DIR}
  ${US3_TURBO_ACCESS_GENERATED_DIR})

# T1.1 — 中间 part < 16MB 应在 Complete 时被拒绝
add_executable(us3_turbo_rtest_rdma_multipart_invalid_part_size
  test_multipart_invalid_part_size.cpp)
target_include_directories(us3_turbo_rtest_rdma_multipart_invalid_part_size
  PRIVATE ${RTEST_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_rtest_rdma_multipart_invalid_part_size
  PRIVATE Us3Turbo::client)

# T1.2 — part_number 重复 / 跳号
add_executable(us3_turbo_rtest_rdma_multipart_part_number_violation
  test_multipart_part_number_violation.cpp)
target_include_directories(us3_turbo_rtest_rdma_multipart_part_number_violation
  PRIVATE ${RTEST_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_rtest_rdma_multipart_part_number_violation
  PRIVATE Us3Turbo::client)

# T1.3 — 单 part = 整对象
add_executable(us3_turbo_rtest_rdma_multipart_single_part
  test_multipart_single_part.cpp)
target_include_directories(us3_turbo_rtest_rdma_multipart_single_part
  PRIVATE ${RTEST_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_rtest_rdma_multipart_single_part
  PRIVATE Us3Turbo::client)
