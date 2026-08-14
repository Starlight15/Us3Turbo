# rtest/ — 回归测试 + 性能基准 + 端到端示例。
# 子目录按类别组织：examples（教学）/ bench（性能）/ regression（正确性）/ scripts（运行脚本）。

# rtest_add_target(<name> <source> [NEEDS_CUDA] [NEEDS_RDMA])
#
# 统一 target 声明——消除子目录 CMakeLists 中重复的
# target_include_directories / target_link_libraries 样板。
#
# NEEDS_CUDA: 链 CUFILE_INCLUDE_DIR + CUDART_LIBRARY（GDS 路径）
# NEEDS_RDMA: 链 IBVERBS_LIBRARY + RDMACM_LIBRARY（直接调用 RDMA API 的 example）
# 两者都不指定时仅链 Us3Turbo::client（纯 host 逻辑、regression 等）
function(rtest_add_target name source)
  cmake_parse_arguments(ARG "NEEDS_CUDA;NEEDS_RDMA" "" "" ${ARGN})
  add_executable(${name} ${source})
  target_include_directories(${name} PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${US3_TURBO_ACCESS_GENERATED_DIR})
  target_link_libraries(${name} PRIVATE Us3Turbo::client)
  if(ARG_NEEDS_CUDA)
    target_include_directories(${name} PRIVATE ${CUFILE_INCLUDE_DIR})
    target_link_libraries(${name} PRIVATE ${CUDART_LIBRARY})
  endif()
  if(ARG_NEEDS_RDMA)
    target_link_libraries(${name} PRIVATE ${IBVERBS_LIBRARY} ${RDMACM_LIBRARY})
  endif()
endfunction()

add_subdirectory(regression)
add_subdirectory(bench)
