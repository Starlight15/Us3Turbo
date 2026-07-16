# GDS regression tests — 每个独立可执行，命名 us3_turbo_rtest_gds_<name>
# 复用 examples 编译模式：CUFILE include + CUDART link。
set(RTEST_PRIVATE_INCLUDES
  ${PROJECT_SOURCE_DIR}
  ${US3_TURBO_ACCESS_GENERATED_DIR})

# T1.1 — 中间 part < 16MB 应在 Complete 时被拒绝
add_executable(us3_turbo_rtest_gds_multipart_invalid_part_size
  test_multipart_invalid_part_size.cpp)
target_include_directories(us3_turbo_rtest_gds_multipart_invalid_part_size
  PRIVATE ${CUFILE_INCLUDE_DIR} ${RTEST_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_rtest_gds_multipart_invalid_part_size
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

# T1.2 — part_number 重复 / 跳号
add_executable(us3_turbo_rtest_gds_multipart_part_number_violation
  test_multipart_part_number_violation.cpp)
target_include_directories(us3_turbo_rtest_gds_multipart_part_number_violation
  PRIVATE ${CUFILE_INCLUDE_DIR} ${RTEST_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_rtest_gds_multipart_part_number_violation
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

# T1.3 — 单 part = 整对象
add_executable(us3_turbo_rtest_gds_multipart_single_part
  test_multipart_single_part.cpp)
target_include_directories(us3_turbo_rtest_gds_multipart_single_part
  PRIVATE ${CUFILE_INCLUDE_DIR} ${RTEST_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_rtest_gds_multipart_single_part
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

# T2.1 — GET 单块对象 crc32c
add_executable(us3_turbo_rtest_gds_get_single_block_crc
  test_get_single_block_crc.cpp)
target_include_directories(us3_turbo_rtest_gds_get_single_block_crc
  PRIVATE ${CUFILE_INCLUDE_DIR} ${RTEST_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_rtest_gds_get_single_block_crc
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

# T2.2 — GET 多块对象 hash
add_executable(us3_turbo_rtest_gds_get_multi_block_hash
  test_get_multi_block_hash.cpp)
target_include_directories(us3_turbo_rtest_gds_get_multi_block_hash
  PRIVATE ${CUFILE_INCLUDE_DIR} ${RTEST_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_rtest_gds_get_multi_block_hash
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})
