add_executable(us3_turbo_proxy
  ${US3_TURBO_ACCESS_CONTROL_PROTO_SRCS}
  ${US3_MESSAGE_PROTO_SRCS}
  src/main.cpp
  src/common/flags.cpp
  src/common/utils.cpp
  src/logging/logger.cpp
  src/logging/access_logger.cpp
  src/api/proxy_service.cpp
  src/service/single_put.cpp
  src/service/multipart.cpp
  src/service/get_object.cpp
  src/index/mongo_upload_index.cpp
  src/index/dbgate_client.cpp
  src/storage/tcp_connection.cpp
  src/storage/ufile_ac_protocol.cpp
  src/storage/ufile_ac_client.cpp)

add_dependencies(us3_turbo_proxy
  us3_turbo_control_plane_proto
  us3_message_proto)

target_include_directories(us3_turbo_proxy
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}
    ${US3_TURBO_ACCESS_GENERATED_DIR}
    ${US3_MESSAGE_GENERATED_DIR})

target_link_libraries(us3_turbo_proxy
  PRIVATE
    us3_turbo_common
    us3_turbo_spdlog
    us3_turbo_brpc
    ${OPENSSL_SSL_STATIC_LIBRARY}
    ${OPENSSL_CRYPTO_STATIC_LIBRARY})

target_compile_options(us3_turbo_proxy
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion)
