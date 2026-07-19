# rtest/bench — 性能基准工具
#
# 单源 multipart_bench.cpp，通过编译期宏 BENCH_GDS 选通路。
# 产出两个独立可执行：
#   us3_turbo_bench_gds_multipart   — GDS 显存分段上传基准（链 CUDA）
set(BENCH_PRIVATE_INCLUDES
  ${PROJECT_SOURCE_DIR}
  ${US3_TURBO_ACCESS_GENERATED_DIR})

add_executable(us3_turbo_bench_gds_multipart multipart_bench.cpp)
target_compile_definitions(us3_turbo_bench_gds_multipart PRIVATE BENCH_GDS=1)
target_include_directories(us3_turbo_bench_gds_multipart
  PRIVATE ${CUFILE_INCLUDE_DIR} ${BENCH_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_bench_gds_multipart
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

