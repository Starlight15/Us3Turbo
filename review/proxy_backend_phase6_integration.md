# Proxy ↔ Backend 交互结构 - 阶段6：集成到 Proxy 服务

## 🎯 目标

将 Proxy 的 PutHandler 集成到 Proxy 的 RPC 服务中，对外提供 GdsPut 和 UcxPut 接口。

---

## 📁 修改文件：`proxy/src/proxy_service.h`

```cpp
#pragma once

#include <memory>

#include "control_plane.pb.h"
#include "proxy/src/put_handler.h"

namespace us3_turbo::proxy {

/**
 * @brief Proxy 端的 RPC 服务实现（对接 Client）。
 * 
 * 职责：
 * 1. 接收 Client 的 GdsPut / UcxPut 请求
 * 2. 调用 PutHandler 进行分块和转发
 * 3. 返回结果给 Client
 */
class ProxyServiceImpl : public us3_turbo::proxy_rpc::ProxyService {
 public:
  /**
   * @param backend_endpoints Backend 节点列表
   * @param block_size 分块大小（默认 5MB）
   */
  explicit ProxyServiceImpl(const std::vector<std::string>& backend_endpoints,
                           uint64_t block_size = 5 * 1024 * 1024);
  ~ProxyServiceImpl() override;

  /**
   * @brief 处理 GDS 路径的对象上传（Client 发起）。
   */
  void GdsPut(google::protobuf::RpcController* controller,
             const us3_turbo::proxy_rpc::ClientProxyPutRequest* request,
             us3_turbo::proxy_rpc::PutPathResult* response,
             google::protobuf::Closure* done) override;

  /**
   * @brief 处理 UCX 路径的对象上传（Client 发起）。
   */
  void UcxPut(google::protobuf::RpcController* controller,
             const us3_turbo::proxy_rpc::ClientProxyPutRequest* request,
             us3_turbo::proxy_rpc::PutPathResult* response,
             google::protobuf::Closure* done) override;

 private:
  std::unique_ptr<PutHandler> put_handler_;
};

}  // namespace us3_turbo::proxy
```

---

## 📁 修改文件：`proxy/src/proxy_service.cpp`

```cpp
#include "proxy/src/proxy_service.h"

#include <brpc/controller.h>
#include <spdlog/spdlog.h>

namespace us3_turbo::proxy {

ProxyServiceImpl::ProxyServiceImpl(const std::vector<std::string>& backend_endpoints,
                                 uint64_t block_size)
    : put_handler_(std::make_unique<PutHandler>(backend_endpoints, block_size)) {
  spdlog::info("ProxyServiceImpl: initialized with {} backends", backend_endpoints.size());
}

ProxyServiceImpl::~ProxyServiceImpl() {
  spdlog::info("ProxyServiceImpl: shutdown");
}

void ProxyServiceImpl::GdsPut(google::protobuf::RpcController* controller,
                             const us3_turbo::proxy_rpc::ClientProxyPutRequest* request,
                             us3_turbo::proxy_rpc::PutPathResult* response,
                             google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);

  const auto& req_id = request->request_id();
  spdlog::info("GdsPut: req={}, bucket={}, key={}, size={}",
               req_id, request->bucket(), request->key(), request->object_size());

  // 转换 protobuf → C++ 结构体
  us3_turbo::client::ClientProxyPutRequest cpp_request;
  cpp_request.request_id = request->request_id();
  cpp_request.bucket = request->bucket();
  cpp_request.key = request->key();
  cpp_request.object_size = request->object_size();
  cpp_request.path = us3_turbo::client::PutDataPath::kGds;
  
  if (request->has_gds_source()) {
    cpp_request.gds_source = us3_turbo::client::GdsDataSource{
      .rdma_token = request->gds_source().rdma_token()
    };
  } else {
    response->set_ok(false);
    response->set_error_code(-1);
    response->set_error_message("missing gds_source");
    spdlog::error("GdsPut: req={}, missing gds_source", req_id);
    return;
  }

  // 调用 PutHandler
  us3_turbo::client::PutPathResult cpp_result;
  bool ok = put_handler_->HandleGdsPut(cpp_request, cpp_result);

  // 转换 C++ 结构体 → protobuf
  response->set_ok(ok);
  if (ok) {
    response->set_etag(cpp_result.etag);
    response->set_crc32c(cpp_result.crc32c);
    response->set_bytes_written(cpp_result.bytes_written);
    spdlog::info("GdsPut: req={}, success, etag={}, crc32c=0x{:08x}",
                 req_id, cpp_result.etag, cpp_result.crc32c);
  } else {
    response->set_error_code(cpp_result.error_code);
    response->set_error_message(cpp_result.error_message);
    spdlog::error("GdsPut: req=, failed: {}", req_id, cpp_result.error_message);
  }
}

void ProxyServiceImpl::UcxPut(google::protobuf::RpcController* controller,
                             const us3_turbo::proxy_rpc::ClientProxyPutRequest* request,
                             us3_turbo::proxy_rpc::PutPathResult* response,
                             google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);

  const auto& req_id = request->request_id();
  spdlog::info("UcxPut: req={}, bucket={}, key={}, size={}",
               req_id, request->bucket(), request->key(), request->object_size());

  // 转换 protobuf → C++ 结构体
  us3_turbo::client::ClientProxyPutRequest cpp_request;
  cpp_request.request_id = request->request_id();
  cpp_request.bucket = request->bucket();
  cpp_request.key = request->key();
  cpp_request.object_size = request->object_size();
  cpp_request.path = us3_turbo::client::PutDataPath::kUcx;
  
  if (request->has_ucx_source()) {
    cpp_request.ucx_source = us3_turbo::client::UcxDataSource{
      .remote_addr = request->ucx_source().remote_addr(),
      .packed_rkey = request->ucx_source().packed_rkey(),
      .client_ucx_addr = request->ucx_source().client_ucx_addr()
    };
  } else {
    response->set_ok(false);
    response->set_error_code(-1);
    response->set_error_message("missing ucx_source");
    spdlog::error("UcxPut: req={}, missing ucx_source", req_id);
    return;
  }

  // 调用 PutHandler
  us3_turbo::client::PutPathResult cpp_result;
  bool ok = put_handler_->HandleUcxPut(cpp_request, cpp_result);

  // 转换 C++ 结构体 → protobuf
  response->set_ok(ok);
  if (ok) {
    response->set_etag(cpp_result.etag);
    response->set_crc32c(cpp_result.crc32c);
    response->set_bytes_written(cpp_result.bytes_written);
    spdlog::info("UcxPut: req={}, success, etag={}, crc32c=0x{:08x}",
                 req_id, cpp_result.etag, cpp_result.crc32c);
  } else {
    response->set_error_code(cpp_result.error_code);
    response->set_error_message(cpp_result.error_message);
    spdlog::error("UcxPut: req={}, failed: ", req_id, cpp_result.error_message);
  }
}

}  // namespace us3_turbo::proxy
```

---

## 📁 修改文件：`proxy/src/main.cpp`（Proxy 启动入口）

```cpp
#include <brpc/server.h>
#include <gflags/gflags.h>
#include <spdlog/spdlog.h>

#include "proxy/src/proxy_service.h"

DEFINE_int32(port, 9090, "Proxy service port");
DEFINE_int32(idle_timeout_s, -1, "Connection idle timeout");
DEFINE_string(backends, "127.0.0.1:8080", "Backend endpoints (comma-separated)");
DEFINE_int32(block_size, 5242880, "Block size in bytes (default 5MB)");

std::vector<std::string> SplitBackends(const std::string& backends) {
  std::vector<std::string> result;
  size_t start = 0;
  while (start < backends.size()) {
    size_t end = backends.find(',', start);
    if (end == std::string::npos) {
      result.push_back(backends.substr(start));
      break;
    }
    result.push_back(backends.substr(start, end - start));
    start = end + 1;
  }
  return result;
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  
  spdlog::set_level(spdlog::level::info);
  spdlog::info("Proxy starting on port {}", FLAGS_port);

  auto backend_endpoints = SplitBackends(FLAGS_backends);
  spdlog::info("Backend endpoints: {}", FLAGS_backends);

  brpc::Server server;
  us3_turbo::proxy::ProxyServiceImpl service(backend_endpoints, FLAGS_block_size);

  if (server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    spdlog::error("Failed to add service");
    return -1;
  }

  brpc::ServerOptions options;
  options.idle_timeout_sec = FLAGS_idle_timeout_s;

  if (server.Start(FLAGS_port, &options) != 0) {
    spdlog::error("Failed to start server on port {}", FLAGS_port);
    return -1;
  }

  spdlog::info("Proxy started, listening on port {}", FLAGS_port);
  server.RunUntilAskedToQuit();

  spdlog::info("Proxy shutting down");
  return 0;
}
```

---

## ✅ 完整启动流程

### 1. 启动 Backend

```bash
# 终端1：启动 Backend（监听 8080）
./backend_server --port=8080

# 终端2：启动第二个 Backend（可选，用于负载均衡）
./backend_server --port=8081
```

### 2. 启动 Proxy

```bash
# 连接到单个 Backend
./proxy_server --port=9090 --backends=127.0.0.1:8080

# 连接到多个 Backend（逗号分隔）
./proxy_server --port=9090 --backends=127.0.0.1:8080,127.0.0.1:8081

# 自定义分块大小（默认 5MB）
./proxy_server --port=9090 --backends=127.0.0.1:8080 --block_size=10485760
```

### 3. Client 调用

```cpp
// Client 端代码
ProxyRpc proxy("127.0.0.1:9090", std::chrono::milliseconds(30000));

ClientProxyPutRequest req;
req.request_id = "client_req_001";
req.bucket = "my-bucket";
req.key = "my-key";
req.object_size = 10485760;  // 10MB
req.path = PutDataPath::kGds;
req.gds_source = GdsDataSource{.rdma_token = "token_abc"};

PutPathResult result;
bool ok = proxy.GdsPut(req.request_id, req.bucket, req.key, 
                       req.object_size, *req.gds_source, result);
if (ok) {
  std::cout << "Upload success! etag=" << result.etag 
            << ", crc32c=0x" << std::hex << result.crc32c << std::endl;
}
```

---

## 📊 数据流向图

```
Client                 Proxy                Backend1              Backend2
  |                      |                      |                     |
  |--GdsPut(10MB)------->|                      |                     |
  |                      |--Split to 2 blocks-->|                     |
  |                      |                      |                     |
  |                      |--Block0(5MB,GDS)---->|                     |
  |                      |                      |--Pull from Client-->|
  |                      |                      |<--Data(5MB)---------|
  |                      |                      |--Write Storage----->|
  |                      |<--Block0 OK----------|                     |
  |                      |                      |                     |
  |                      |--Block1(5MB,GDS)-------------------->|     |
  |                      |                      |                |    |
  |                      |                      |<--Pull from Client--|
  |                      |                      |                |    |
  |                      |                      |--Write Storage----->|
  |                      |<--Block1 OK----------------------|          |
  |                      |                      |                     |
  |<--PutPathResult------|                      |                     |
  |   (etag,crc32c)      |                      |                     |
```

---

## ✅ 验证步骤

1. **编译 Proxy**：
   ```bash
   g++ -std=c++17 -I. -I<brpc_include> -pthread \
     proxy/src/proxy_service.cpp proxy/src/main.cpp \
     proxy/src/put_handler.cpp proxy/src/backend_rpc.cpp \
     -o proxy_server \
     -lbrpc -lprotobuf -lgflags -lpthread
   ```

2. **端到端测试**：
   - 启动 Backend（8080）
   - 启动 Proxy（9090，连接到 8080）
   - Client 调用 Proxy 的 GdsPut（10MB 对象）
   - 观察日志：Proxy 拆分为 2 个 block，Backend 收到 2 次 PutBlock 请求

3. **日志检查**：
   ```
   # Proxy 日志
   [info] GdsPut: req=client_req_001, bucket=my-bucket, key=my-key, size=10485760
   [info] HandleGdsPut: split to 2 blocks
   [info] PutBlock: block0 sent to backend 127.0.0.1:8080
   [info] PutBlock: block1 sent to backend 127.0.0.1:8080
   [info] GdsPut: req=client_req_001, success, etag=etag_xxx

   # Backend 日志
   [info] PutBlock: req=client_req_001_block0_gds, size=5242880
   [info] PutBlock: req=client_req_001_block0_gds, success
   [info] PutBlock: req=client_req_001_block1_gds, size=5242880
   [info] PutBlock: req=client_req_001_block1_gds, success
   ```

---

## 📝 注意事项

1. ✅ Proxy 服务同时提供 GdsPut 和 UcxPut 两个接口
2. ✅ 支持多 Backend 负载均衡（轮询策略）
3. ✅ 日志包含完整的 request_id 链路追踪
4. ✅ 错误处理完善（缺少 source / Backend 失败）
5. ⚠️ 当前未实现 ALL 模式（Client 同时走 GDS+UCX），需要在 Client 端并发调用 GdsPut 和 UcxPut
