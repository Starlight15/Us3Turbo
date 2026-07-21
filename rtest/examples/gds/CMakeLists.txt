# examples/gds/ — GDS 端到端验证示例（device 显存，链 CUDA）。
set(GDS_EXAMPLE_PRIVATE_INCLUDES
  ${PROJECT_SOURCE_DIR}
  ${US3_TURBO_ACCESS_GENERATED_DIR})

add_executable(us3_turbo_gds_put_example gds_put_example.cpp)
target_include_directories(us3_turbo_gds_put_example
  PRIVATE ${CUFILE_INCLUDE_DIR} ${GDS_EXAMPLE_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_gds_put_example
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

add_executable(us3_turbo_gds_get_example gds_get_example.cpp)
target_include_directories(us3_turbo_gds_get_example
  PRIVATE ${CUFILE_INCLUDE_DIR} ${GDS_EXAMPLE_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_gds_get_example
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})

add_executable(us3_turbo_gds_multipart_example gds_multipart_example.cpp)
target_include_directories(us3_turbo_gds_multipart_example
  PRIVATE ${CUFILE_INCLUDE_DIR} ${GDS_EXAMPLE_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_gds_multipart_example
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})
