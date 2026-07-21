# examples/rdma/ — RDMA (libibverbs) 端到端示例（host 内存，无 CUDA 依赖）。
set(RDMA_EXAMPLE_PRIVATE_INCLUDES
  ${PROJECT_SOURCE_DIR}
  ${US3_TURBO_ACCESS_GENERATED_DIR})

add_executable(us3_turbo_rdma_put_example rdma_put_example.cpp)
target_include_directories(us3_turbo_rdma_put_example
  PRIVATE ${RDMA_EXAMPLE_PRIVATE_INCLUDES})
target_link_libraries(us3_turbo_rdma_put_example
  PRIVATE Us3Turbo::client ${IBVERBS_LIBRARY} ${RDMACM_LIBRARY})
