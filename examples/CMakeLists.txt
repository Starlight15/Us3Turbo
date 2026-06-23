# examples/ — GDS PUT 端到端验证用例(本工程仅保留此例)。
#
# 编译产物:
#   build/examples/us3_turbo_gds_put_example

add_executable(us3_turbo_gds_put_example gds/gds_put_example.cpp)

target_include_directories(us3_turbo_gds_put_example PRIVATE ${CUFILE_INCLUDE_DIR})

target_link_libraries(us3_turbo_gds_put_example
  PRIVATE Us3Turbo::client ${CUDART_LIBRARY})
