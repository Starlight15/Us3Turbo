add_executable(us3_turbo_backend
  ${US3_TURBO_ACCESS_CONTROL_PROTO_SRCS}
  src/main.cpp
  src/backend_gds_sink.cpp
  src/backend_data_plane_service.cpp
  # GDS 数据面本地组件(原 gateway 借用文件,已并入 backend/src):
  #   gds/buffer_pool.cpp / gds/cuobj_resources.cpp —— cuObjServer pinned buffer 池与 RAII
  #   common/crc32c.cpp —— 自包含 CRC32C,供 PUT 丢弃前算校验和做端到端校验
  src/gds/buffer_pool.cpp
  src/gds/cuobj_resources.cpp
  src/common/crc32c.cpp
  # RDMA(UCX)数据面:与 gds 链路独立,不复用 gds 资源。
  src/rdma/ucx_resources.cpp
  src/rdma/ucx_sink.cpp)

add_dependencies(us3_turbo_backend
  us3_turbo_control_plane_proto)

target_include_directories(us3_turbo_backend
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}
    ${US3_TURBO_ACCESS_GENERATED_DIR}
    ${CUOBJCLIENT_INCLUDE_DIR}
    ${CUFILE_INCLUDE_DIR})

target_link_libraries(us3_turbo_backend
  PRIVATE
    us3_turbo_common
    us3_turbo_spdlog
    us3_turbo_brpc
    ${CUOBJSERVER_LIBRARY}
    ${IBVERBS_LIBRARY}
    ${CUDART_LIBRARY}
    ${UCP_LIBRARY} ${UCS_LIBRARY} ${UCT_LIBRARY} ${UCM_LIBRARY})

if(UNIX AND NOT APPLE)
  target_link_libraries(us3_turbo_backend PRIVATE dl)
endif()

target_compile_options(us3_turbo_backend
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion)
