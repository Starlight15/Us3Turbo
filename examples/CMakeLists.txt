# examples/ — GDS PUT + UCX PUT + 分段上传 端到端验证与基准测试用例。
#
# 编译产物:
#   build/examples/us3_turbo_gds_put_example        — GDS 单次 PUT 端到端
#   build/examples/us3_turbo_gds_bench_example      — GDS 可指定大小/数量/并发的基准
#   build/examples/us3_turbo_ucx_put_example        — UCX 单次 PUT 端到端
#   build/examples/us3_turbo_gds_multipart_example  — GDS 分段上传端到端
#   build/examples/us3_turbo_ucx_multipart_example  — UCX 分段上传端到端
#   build/examples/us3_turbo_multipart_bench_example— 单步 vs 分段 PUT 对比基准
#   build/examples/us3_turbo_gds_get_example       — GDS 单步+分段 PUT 后 GET 读回验证
#   build/examples/us3_turbo_ucx_get_example       — UCX 单步+分段 PUT 后 GET 读回验证

# 公共 include：仓库根(client/src/common/request.h 等以仓库根为基的路径)
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
add_executable(us3_turbo_ucx_put_example gds/ucx_put_example.cpp)

target_include_directories(us3_turbo_ucx_put_example
  PRIVATE ${EXAMPLES_PRIVATE_INCLUDES})

target_link_libraries(us3_turbo_ucx_put_example
  PRIVATE Us3Turbo::client)

# 分段上传 example：GDS / UCX 端到端 + 单步 vs 分段基准。
add_executable(us3_turbo_gds_multipart_example gds/gds_multipart_example.cpp)

target_include_directories(us3_turbo_gds_multipart_example
  PRIVATE ${CUFILE_INCLUDE_DIR} ${EXAMPLES_PRIVATE_INCLUDES})

target_link_libraries(us3_turbo_gds_multipart_example
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

add_executable(us3_turbo_ucx_multipart_example gds/ucx_multipart_example.cpp)

target_include_directories(us3_turbo_ucx_multipart_example
  PRIVATE ${EXAMPLES_PRIVATE_INCLUDES})

target_link_libraries(us3_turbo_ucx_multipart_example
  PRIVATE Us3Turbo::client)

add_executable(us3_turbo_multipart_bench_example gds/multipart_bench_example.cpp)

target_include_directories(us3_turbo_multipart_bench_example
  PRIVATE ${CUFILE_INCLUDE_DIR} ${EXAMPLES_PRIVATE_INCLUDES})

target_link_libraries(us3_turbo_multipart_bench_example
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

# GDS PUT + GET 端到端验证：单步/分段上传 → StatObject → GetObjectGds → 逐字节比对。
add_executable(us3_turbo_gds_get_example gds/gds_get_example.cpp)

target_include_directories(us3_turbo_gds_get_example
  PRIVATE ${CUFILE_INCLUDE_DIR} ${EXAMPLES_PRIVATE_INCLUDES})

target_link_libraries(us3_turbo_gds_get_example
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

# UCX PUT + GET 端到端验证：单步/分段上传 → StatObject → GetObjectUcx → 逐字节比对。
# host 内存，无 CUDA 依赖。
add_executable(us3_turbo_ucx_get_example gds/ucx_get_example.cpp)

target_include_directories(us3_turbo_ucx_get_example
  PRIVATE ${EXAMPLES_PRIVATE_INCLUDES})

target_link_libraries(us3_turbo_ucx_get_example
  PRIVATE Us3Turbo::client)
