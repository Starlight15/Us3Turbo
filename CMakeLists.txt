cmake_minimum_required(VERSION 3.24)
project(Us3Turbo VERSION 0.1.0 LANGUAGES CXX)

# Us3Turbo — GDS-only 精简工程。
# 由 Us3Turbo 抽取 proxy / client(原 client-new) / backend / third_party
# 及 gds_put_example 而来。只保留 GDS PUT 链路:控制面 brpc(proxy)+
# 数据面 cuObjServer(backend)+ 客户端 GDS 通路。无 RDMA / HTTP / 分段上传。

# 强制 out-of-tree build。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "In-source builds are not allowed. "
    "Run cmake from a separate build directory, e.g. "
    "`cmake -S ${CMAKE_SOURCE_DIR} -B ${CMAKE_SOURCE_DIR}/build` "
    "or use ./do_make.sh.")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(US3_TURBO_ACCESS_BUILD_EXAMPLES "Build Us3Turbo examples" ON)
option(US3_TURBO_ACCESS_USE_SPDLOG_HEADER_ONLY "Use spdlog header-only target" ON)

set(FUSION_ACCESS_DEPS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/install"
    CACHE PATH "Offline dependency install root (default: in-repo third_party/install)")
set(US3_TURBO_ACCESS_PROTOBUF_ROOT "${FUSION_ACCESS_DEPS_ROOT}/protobuf-25.4-install"
    CACHE PATH "Protobuf static-library install prefix")
set(US3_TURBO_ACCESS_BRPC_ROOT "${FUSION_ACCESS_DEPS_ROOT}/brpc-1.11.0-static"
    CACHE PATH "brpc static-library install prefix")

find_package(spdlog REQUIRED
  PATHS "${FUSION_ACCESS_DEPS_ROOT}/spdlog-install"
  NO_DEFAULT_PATH)
find_package(nlohmann_json REQUIRED)
find_package(Protobuf CONFIG REQUIRED
  PATHS
    ${US3_TURBO_ACCESS_PROTOBUF_ROOT}
    ${US3_TURBO_ACCESS_PROTOBUF_ROOT}/lib/cmake/protobuf
    ${US3_TURBO_ACCESS_PROTOBUF_ROOT}/lib64/cmake/protobuf
  NO_DEFAULT_PATH)

add_library(us3_turbo_spdlog INTERFACE)
if(US3_TURBO_ACCESS_USE_SPDLOG_HEADER_ONLY AND TARGET spdlog::spdlog_header_only)
  target_link_libraries(us3_turbo_spdlog INTERFACE spdlog::spdlog_header_only)
else()
  target_link_libraries(us3_turbo_spdlog INTERFACE spdlog::spdlog)
endif()

# 公共头(error_code.h)所在目录。
add_library(us3_turbo_common INTERFACE)
target_include_directories(us3_turbo_common INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_library(BRPC_STATIC_LIBRARY NAMES libbrpc.a
  HINTS ${US3_TURBO_ACCESS_BRPC_ROOT}/lib REQUIRED)
find_library(GFLAGS_STATIC_LIBRARY NAMES libgflags.a
  HINTS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
find_library(LEVELDB_STATIC_LIBRARY NAMES libleveldb.a
  HINTS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
find_library(SNAPPY_STATIC_LIBRARY NAMES libsnappy.a
  HINTS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
find_library(ZLIB_STATIC_LIBRARY NAMES libz.a
  HINTS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
find_library(OPENSSL_SSL_STATIC_LIBRARY NAMES libssl.a
  HINTS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
find_library(OPENSSL_CRYPTO_STATIC_LIBRARY NAMES libcrypto.a
  HINTS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
find_library(PROTOBUF_STATIC_LIBRARY NAMES libprotobuf.a
  HINTS ${US3_TURBO_ACCESS_PROTOBUF_ROOT}/lib ${US3_TURBO_ACCESS_PROTOBUF_ROOT}/lib64 REQUIRED)
find_library(PROTOC_STATIC_LIBRARY NAMES libprotoc.a
  HINTS ${US3_TURBO_ACCESS_PROTOBUF_ROOT}/lib ${US3_TURBO_ACCESS_PROTOBUF_ROOT}/lib64 REQUIRED)
find_library(UTF8_RANGE_STATIC_LIBRARY NAMES libutf8_range.a
  HINTS ${US3_TURBO_ACCESS_PROTOBUF_ROOT}/lib ${US3_TURBO_ACCESS_PROTOBUF_ROOT}/lib64 REQUIRED)
find_library(UTF8_VALIDITY_STATIC_LIBRARY NAMES libutf8_validity.a
  HINTS ${US3_TURBO_ACCESS_PROTOBUF_ROOT}/lib ${US3_TURBO_ACCESS_PROTOBUF_ROOT}/lib64 REQUIRED)
file(GLOB ABSEIL_STATIC_LIBRARIES "${US3_TURBO_ACCESS_PROTOBUF_ROOT}/lib/libabsl_*.a")
if(NOT ABSEIL_STATIC_LIBRARIES)
  file(GLOB ABSEIL_STATIC_LIBRARIES "${US3_TURBO_ACCESS_PROTOBUF_ROOT}/lib64/libabsl_*.a")
endif()
if(NOT ABSEIL_STATIC_LIBRARIES)
  message(FATAL_ERROR "No abseil static libraries found under ${US3_TURBO_ACCESS_PROTOBUF_ROOT}")
endif()
list(SORT ABSEIL_STATIC_LIBRARIES)

find_library(IBVERBS_LIBRARY NAMES ibverbs REQUIRED)
find_library(RDMACM_LIBRARY NAMES rdmacm REQUIRED)
find_library(CUOBJSERVER_LIBRARY NAMES cuobjserver REQUIRED)
find_library(CUOBJCLIENT_LIBRARY NAMES cuobjclient
  HINTS
    /usr/local/cuda/targets/x86_64-linux/lib
    /usr/local/cuda-13.1/targets/x86_64-linux/lib
    /usr/local/cuda-13.2/targets/x86_64-linux/lib
  REQUIRED)
find_library(CUDART_LIBRARY NAMES cudart_static cudart
  HINTS
    /usr/local/cuda/targets/x86_64-linux/lib
    /usr/local/cuda-12/targets/x86_64-linux/lib
    /usr/local/cuda-12.6/targets/x86_64-linux/lib
    /usr/local/cuda-13.2/targets/x86_64-linux/lib
  REQUIRED)
get_filename_component(CUDART_LIBRARY_DIR ${CUDART_LIBRARY} DIRECTORY)
link_directories(${CUDART_LIBRARY_DIR})

find_path(CUOBJCLIENT_INCLUDE_DIR NAMES cuobjclient.h
  HINTS
    /usr/local/cuda/targets/x86_64-linux/include
    /usr/local/cuda-13.2/targets/x86_64-linux/include
  REQUIRED)
find_path(CUFILE_INCLUDE_DIR NAMES cufile.h
  HINTS
    /usr/local/cuda/targets/x86_64-linux/include
    /usr/local/cuda-12.6/targets/x86_64-linux/include
    /usr/local/cuda-13.2/targets/x86_64-linux/include
  REQUIRED)

add_library(us3_turbo_brpc INTERFACE)
target_include_directories(us3_turbo_brpc INTERFACE ${US3_TURBO_ACCESS_BRPC_ROOT}/include)
target_link_libraries(us3_turbo_brpc INTERFACE
  -Wl,--start-group
    ${BRPC_STATIC_LIBRARY}
    ${PROTOBUF_STATIC_LIBRARY}
    ${PROTOC_STATIC_LIBRARY}
    ${UTF8_RANGE_STATIC_LIBRARY}
    ${UTF8_VALIDITY_STATIC_LIBRARY}
    ${ABSEIL_STATIC_LIBRARIES}
    ${LEVELDB_STATIC_LIBRARY}
    ${SNAPPY_STATIC_LIBRARY}
    ${GFLAGS_STATIC_LIBRARY}
    ${OPENSSL_SSL_STATIC_LIBRARY}
    ${OPENSSL_CRYPTO_STATIC_LIBRARY}
    ${ZLIB_STATIC_LIBRARY}
  -Wl,--end-group
  dl rt pthread)

# ============================================================
#  control_plane.proto(本工程唯一 proto;无 rdma / http proto)
# ============================================================
set(US3_TURBO_ACCESS_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(US3_TURBO_ACCESS_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${US3_TURBO_ACCESS_GENERATED_DIR})

set(US3_TURBO_ACCESS_CONTROL_PROTO_FILE ${US3_TURBO_ACCESS_PROTO_DIR}/control_plane.proto)
set(US3_TURBO_ACCESS_CONTROL_PROTO_SRCS ${US3_TURBO_ACCESS_GENERATED_DIR}/control_plane.pb.cc)
set(US3_TURBO_ACCESS_CONTROL_PROTO_HDRS ${US3_TURBO_ACCESS_GENERATED_DIR}/control_plane.pb.h)
add_custom_command(
  OUTPUT ${US3_TURBO_ACCESS_CONTROL_PROTO_SRCS} ${US3_TURBO_ACCESS_CONTROL_PROTO_HDRS}
  COMMAND protobuf::protoc
  ARGS --cpp_out=${US3_TURBO_ACCESS_GENERATED_DIR} -I ${US3_TURBO_ACCESS_PROTO_DIR} ${US3_TURBO_ACCESS_CONTROL_PROTO_FILE}
  DEPENDS ${US3_TURBO_ACCESS_CONTROL_PROTO_FILE}
  VERBATIM)
add_custom_target(us3_turbo_control_plane_proto
  DEPENDS ${US3_TURBO_ACCESS_CONTROL_PROTO_SRCS} ${US3_TURBO_ACCESS_CONTROL_PROTO_HDRS})

add_subdirectory(client)
add_subdirectory(proxy)
add_subdirectory(backend)

if(US3_TURBO_ACCESS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
