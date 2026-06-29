# client — GDS-only 通路分离实现（Mode B：单 channel 指向 proxy）
#
# 消除旧 client 的过度抽象(TransferRouter / TransferPath / ClientCore /
# UploadCoordinator 等)。本期范围:只实现 GDS PUT(跑通 gds_put_example),
# 不实现分段上传、HeadObject、GetObject、RDMA。
#
# Mode B：MetaRpc + ChunkRpc 合并为单 ProxyRpc（GdsPut / RdmaPut
# 共用一条到 proxy 的 brpc channel）。
#
# proto 生成产物(control_plane.pb.*)由根 CMake 生成在
# US3_TURBO_ACCESS_GENERATED_DIR,直接 include 即可。

add_library(us3_turbo_client
  ${US3_TURBO_ACCESS_CONTROL_PROTO_SRCS}
  src/client.cpp
  src/contracts/request_builder.cpp
  src/proxy_rpc.cpp
  src/data/crc32c.cpp
  src/gds_transport/gds_memory_manager.cpp
  src/rdma_transport/ucx_memory_manager.cpp)

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
    ${UCP_LIBRARY} ${UCS_LIBRARY} ${UCT_LIBRARY} ${UCM_LIBRARY})

target_compile_options(us3_turbo_client
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion)
