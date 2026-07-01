# examples/ — GDS PUT + UCX PUT 端到端验证与基准测试用例。
#
# 编译产物:
#   build/examples/us3_turbo_gds_put_example    — GDS 单次 PUT 端到端
#   build/examples/us3_turbo_gds_bench_example  — GDS 可指定大小/数量/并发的基准
#   build/examples/us3_turbo_rdma_put_example   — UCX 单次 PUT 端到端

# 公共 include：仓库根（client/src/contracts/put_request.h 等以仓库根为基的路径）
# + generated（control_plane.pb.h）。与 client 库的 PRIVATE include 一致。
set(EXAMPLES_PRIVATE_INCLUDES
  ${PROJECT_SOURCE_DIR}
  ${US3_TURBO_ACCESS_GENERATED_DIR})

add_executable(us3_turbo_gds_put_example gds/gds_put_example.cpp)

target_include_directories(us3_turbo_gds_put_example
  PRIVATE ${CUFILE_INCLUDE_DIR} ${EXAMPLES_PRIVATE_INCLUDES})

target_link_libraries(us3_turbo_gds_put_example
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

add_executable(us3_turbo_gds_bench_example gds/gds_bench_example.cpp)

target_include_directories(us3_turbo_gds_bench_example
  PRIVATE ${CUFILE_INCLUDE_DIR} ${EXAMPLES_PRIVATE_INCLUDES})

target_link_libraries(us3_turbo_gds_bench_example
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

# UCX 链路 example：host 内存，无 CUDA 依赖（UCX 经 client lib 链接）。
add_executable(us3_turbo_rdma_put_example gds/rdma_put_example.cpp)

target_include_directories(us3_turbo_rdma_put_example
  PRIVATE ${EXAMPLES_PRIVATE_INCLUDES})

target_link_libraries(us3_turbo_rdma_put_example
  PRIVATE Us3Turbo::client)
