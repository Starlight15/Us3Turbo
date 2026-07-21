# bench/gds/ — GDS 性能基准（device 显存，链 CUDA）。
set(BENCH_GDS_PRIVATE_INCLUDES
  ${PROJECT_SOURCE_DIR}
  ${US3_TURBO_ACCESS_GENERATED_DIR})

add_executable(us3_turbo_bench_gds_put gds_put_bench.cpp)
target_include_directories(us3_turbo_bench_gds_put
  PRIVATE ${CUFILE_INCLUDE_DIR} ${BENCH_GDS_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_bench_gds_put
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

add_executable(us3_turbo_bench_gds_multipart gds_multipart_bench.cpp)
target_include_directories(us3_turbo_bench_gds_multipart
  PRIVATE ${CUFILE_INCLUDE_DIR} ${BENCH_GDS_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_bench_gds_multipart
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})
