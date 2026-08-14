# utility/ — 对象存储命令行工具 (上传 / 下载指定对象)。
# 取代原 rtest/examples/ 的教学示例,合并为单一可执行 us3_turbo_utility。
# 编译产物: build/utility/us3_turbo_utility
#
# 依赖:Us3Turbo::client(透传 ibverbs/rdmacm,及 GDS 时的 CUDA/cuobj)。
# GDS 通路代码在 utility_main.cpp 内以 #ifdef US3_TURBO_ACCESS_ENABLE_GDS 门控。

add_executable(us3_turbo_utility utility_main.cpp)
target_include_directories(us3_turbo_utility PRIVATE
  ${PROJECT_SOURCE_DIR}
  ${US3_TURBO_ACCESS_GENERATED_DIR})
target_link_libraries(us3_turbo_utility PRIVATE Us3Turbo::client)
if(US3_TURBO_ACCESS_ENABLE_GDS)
  target_include_directories(us3_turbo_utility PRIVATE ${CUDA_INCLUDE_DIR})
endif()
