# Proxy ↔ Backend 交互结构 - 阶段3：实现 Proxy 端 BackendRpc

## 🎯 目标

实现 Proxy 端的 Backend RPC 客户端，用于向 Backend 发起 PutBlock 请求。

---

## 📁 新建文件：`proxy/src/backend_rpc.h`

```cpp
#pragma once

#include <chrono>
#include <string>

#include "proxy/src/common/rpc_base.h"
#include "proxy/src/contracts/set_put_request.h"

namespace us3_turbo::proxy {

/**
 * @brief Proxy 向 Backend 发起 RPC 的客户端。
 * 
 * 用于单块上传：Proxy 将 client 的对象上传请求拆分为多个 block，逐个调用
 * Backend 的 PutBlock RPC。Backend 根据 source 描述符主动拉取数据。
 * 
 * 线程安全：brpc::Channel 线程安全，可被多线程并发调用。
 */
class BackendRpc : public RpcBase {
 public:
  /**
   * @param endpoint Backend 地址（格式："ip:port"）
   * @param timeout 单次 RPC 超时时间
   */
  BackendRpc(const std::string& endpoint, std::chrono::milliseconds timeout)
      : RpcBase(endpoint, timeout, "backend") {}

  /**
   * @brief 上传单个 block（Backend 主动拉取数据）。
   * 
   * @param request 完整的块上传请求（包含对象元数据、块元数据、数据源描述符）
   * @param response 返回块上传结果（bytes_written / crc32c / zero_padding）
   * @return true 成功，false 失败（RPC 错误或 Backend 返回 ok=false）
   */
  [[nodiscard]] bool PutBlock(const ProxySetPutBlockRequest& request,
                              ProxySetPutBlockResponse& response) const;
};

}  // namespace us3_turbo::proxy
```

---

## 📁 新建文件：`proxy/src/backend_rpc.cpp`

```cpp
#include "proxy/src/backend_rpc.h"

#include <brpc/controller.h>
#include <brpc/errno.pb.h>
#include <spdlog/spdlog.h>

#include "backend_service.pb.h"
#include "proxy/src/contracts/set_put_converter.h"

namespace us3_turbo::proxy {

bool BackendRpc::PutBlock(const ProxySetPutBlockRequest& request,
                         ProxySetPutBlockResponse& response) const {
  if (!ok()) {
    spdlog::error("PutBlock (req={}): backend channel not ready: {}",
                  request.request_id, init_error());
    return false;
  }

  brpc::Controller controller;
  ApplyTimeout(controller, timeout_);

  // C++ 结构体 → protobuf
  us3_turbo::backend::ProxySetPutBlockRequest rpc_request;
  ToProto(request, &rpc_request);

  us3_turbo::backend::ProxySetPutBlockResponse rpc_response;
  stub()->PutBlock(&controller, &rpc_request, &rpc_response, nullptr);

  if (controller.Failed()) {
    const bool is_timeout =
        (controller.ErrorCode() == brpc::ERPCTIMEDOUT) ||
        (controller.ErrorCode() == ETIMEDOUT);
    spdlog::error("{} (req={}): failed to execute PutBlock RPC: {}",
                  is_timeout ? "timeout" : "rpc-error",
                  request.request_id, controller.ErrorText());
    return false;
  }

  // protobuf → C++ 结构体
  FromProto(rpc_response, response);

  if (!response.ok) {
    spdlog::warn("PutBlock (req={}): backend returned error: code={}, msg={}",
                 request.request_id, response.error_code, response.error_message);
    return false;
  }

  return true;
}

}  // namespace us3_turbo::proxy
```

---

## 📝 依赖的 RpcBase（参考 client 的实现）

### 文件：`proxy/src/common/rpc_base.h`（如果没有则新建）

```cpp
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <brpc/channel.h>

namespace us3_turbo::proxy {

/**
 * @brief RPC 客户端基类（管理 brpc::Channel 生命周期）。
 */
class RpcBase {
 public:
  RpcBase(const std::string& endpoint, std::chrono::milliseconds timeout,
          const std::string& service_name);
  virtual ~RpcBase() = default;

  [[nodiscard]] bool ok() const { return init_ok_; }
  [[nodiscard]] const std::string& init_error() const { return init_error_; }

 protected:
  void ApplyTimeout(brpc::Controller& controller,
                   std::chrono::milliseconds timeout) const {
    controller.set_timeout_ms(timeout.count());
  }

  brpc::Channel* stub() const { return channel_.get(); }

  std::chrono::milliseconds timeout_;

 private:
  std::unique_ptr<brpc::Channel> channel_;
  bool init_ok_{false};
  std::string init_error_;
};

}  // namespace us3_turbo::proxy
```

### 文件：`proxy/src/common/rpc_base.cpp`（如果没有则新建）

```cpp
#include "proxy/src/common/rpc_base.h"

#include <brpc/channel.h>
#include <spdlog/spdlog.h>

namespace us3_turbo::proxy {

RpcBase::RpcBase(const std::string& endpoint, std::chrono::milliseconds timeout,
                 const std::string& service_name)
    : timeout_(timeout), channel_(std::make_unique<brpc::Channel>()) {
  brpc::ChannelOptions options;
  options.timeout_ms = timeout.count();
  options.max_retry = 0;

  if (channel_->Init(endpoint.c_str(), &options) != 0) {
    init_error_ = "failed to init brpc channel to " + endpoint;
    spdlog::error("{} RPC: {}", service_name, init_error_);
    return;
  }

  init_ok_ = true;
  spdlog::info("{} RPC: channel ready (endpoint={})", service_name, endpoint);
}

}  // namespace us3_turbo::proxy
```

---

## ✅ 验证步骤

1. **编译通过**：
   ```bash
   g++ -std=c++17 -I. -I<brpc_include> -I<protobuf_include> \
     -c proxy/src/backend_rpc.cpp -o proxy/src/backend_rpc.o
   ```

2. **逻辑检查**：
   - ✅ 使用 `ToProto` 转换请求
   - ✅ 使用 `FromProto` 转换响应
   - ✅ 处理 RPC 超时和错误
   - ✅ 检查 `response.ok` 判断 Backend 是否成功

3. **单元测试**（伪代码）：
   ```cpp
   BackendRpc backend("127.0.0.1:8080", std::chrono::milliseconds(5000));
   
   ProxySetPutBlockRequest req;
   req.request_id = "test_req_block0";
   req.set_id = 42;
   req.object_id = "obj_123";
   req.block_id = "obj_123_0";
   req.block_no = 0;
   req.block_size = 1024;
   req.source = SetPullSource::Gds("token_abc", 0);
   
   ProxySetPutBlockResponse resp;
   bool ok = backend.PutBlock(req, resp);
   // 验证 ok 和 resp 的字段
   ```

---

## 📝 注意事项

1. ✅ `BackendRpc` 继承 `RpcBase`，复用 channel 管理逻辑
2. ✅ 日志包含 `request_id`，便于全链路追踪
3. ✅ 区分 RPC 错误和 Backend 业务错误（`controller.Failed()` vs `response.ok`）
4. ✅ 线程安全（brpc::Channel 本身线程安全）
