# examples/ — GDS PUT 端到端验证与基准测试用例。
#
# 编译产物:
#   build/examples/us3_turbo_gds_put_example    — 单次 PUT 端到端
#   build/examples/us3_turbo_gds_bench_example  — 可指定大小/数量/并发的基准

add_executable(us3_turbo_gds_put_example gds/gds_put_example.cpp)

target_include_directories(us3_turbo_gds_put_example PRIVATE ${CUFILE_INCLUDE_DIR})

target_link_libraries(us3_turbo_gds_put_example
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

add_executable(us3_turbo_gds_bench_example gds/gds_bench_example.cpp)

target_include_directories(us3_turbo_gds_bench_example PRIVATE ${CUFILE_INCLUDE_DIR})

target_link_libraries(us3_turbo_gds_bench_example
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})
