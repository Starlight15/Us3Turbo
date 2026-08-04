# client — Mode B 双链路(GDS + RDMA)对象存储客户端:单 channel 指向 proxy。
#
# 支持单步 PUT、分段上传、GET，GDS(device 显存)+RDMA(host 内存)双通路。
# Mode B:MetaRpc + ChunkRpc 合并为单 ProxyRpc(GdsPut / RdmaPut / 分段 / Get
# 共用一条到 proxy 的 brpc channel)。
#
# 目录按职责划分:common/(契约+链路无关工具,零 brpc 依赖)、
# rpc/(brpc 控制面布线)、memory_manager/(buffer 注册)、transport/(channel)。
# proto 生成产物(control_plane.pb.*)由根 CMake 生成在
# US3_TURBO_ACCESS_GENERATED_DIR,直接 include 即可。

add_library(us3_turbo_client
  ${US3_TURBO_ACCESS_CONTROL_PROTO_SRCS}
  src/client.cpp
  src/rpc/proxy_rpc.cpp
  src/common/crc32c.cpp
  src/memory_manager/gds_memory_manager.cpp
  src/memory_manager/rdma_memory_manager.cpp
  src/memory_manager/rdma_qp.cpp
  src/transport/gds_put_channel.cpp
  src/transport/gds_get_channel.cpp
  src/transport/rdma_put_channel.cpp
  src/transport/rdma_get_channel.cpp)

add_dependencies(us3_turbo_client
  us3_turbo_control_plane_proto)

add_library(Us3Turbo::client ALIAS us3_turbo_client)

set_target_properties(us3_turbo_client
  PROPERTIES OUTPUT_NAME us3_turbo_client)

target_include_directories(us3_turbo_client
  PUBLIC
    ${PROJECT_SOURCE_DIR}/client/include
  PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/client/src
    ${US3_TURBO_ACCESS_GENERATED_DIR}
    ${CUOBJCLIENT_INCLUDE_DIR}
    ${CUFILE_INCLUDE_DIR})

target_link_libraries(us3_turbo_client
  PUBLIC
    us3_turbo_common
    us3_turbo_spdlog
    nlohmann_json::nlohmann_json
    us3_turbo_brpc
    ${CUOBJCLIENT_LIBRARY}
    ${CUDART_LIBRARY}
    ${CUDA_DRIVER_LIBRARY}
    ${UCP_LIBRARY} ${UCS_LIBRARY} ${UCT_LIBRARY} ${UCM_LIBRARY}
    ${IBVERBS_LIBRARY} ${RDMACM_LIBRARY})

target_compile_options(us3_turbo_client
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion)
