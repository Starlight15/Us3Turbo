# Proxy ↔ Backend 交互结构 - 阶段5：实现 Backend 端服务

## 🎯 目标

实现 Backend 端的 PutBlock RPC 服务，接收 Proxy 的请求，主动拉取数据并写入存储系统。

---

## 📁 新建文件：`backend/src/put_block_service.h`

```cpp
#pragma once

#include "backend_service.pb.h"

namespace us3_turbo::backend {

/**
 * @brief Backend 端的 PutBlock RPC 服务实现。
 * 
 * 职责：
 * 1. 接收 Proxy 的 PutBlock 请求
 * 2. 根据 source 模式（GDS 或 UCX）拉取 Client 的数据
 * 3. 计算 CRC32C 校验
 * 4. 写入存储系统（set_id 对应的存储节点）
 * 5. 返回写入结果
 */
class PutBlockServiceImpl : public BackendService {
 public:
  PutBlockServiceImpl();
  ~PutBlockServiceImpl() override;

  /**
   * @brief 处理单块上传请求。
   * 
   * @param controller brpc 控制器
   * @param request Proxy 的块上传请求
   * @param response 返回块上传结果
   * @param done RPC 回调（brpc 要求）
   */
  void PutBlock(google::protobuf::RpcController* controller,
               const ProxySetPutBlockRequest* request,
               ProxySetPutBlockResponse* response,
               google::protobuf::Closure* done) override;

 private:
  // 从 GDS token 拉取数据
  [[nodiscard]] bool PullDataGds(const SetPullSource& source,
                                 uint64_t block_size,
                                 std::vector<uint8_t>& buffer,
                                 std::string& error_message);

  // 从 UCX remote memory 拉取数据
  [[nodiscard]] bool PullDataUcx(const SetPullSource& source,
                                 uint64_t block_size,
                                 std::vector<uint8_t>& buffer,
                                 std::string& error_message);

  // 计算 CRC32C（包含 zero_padding）
  [[nodiscard]] uint32_t ComputeCrc32c(const std::vector<uint8_t>& buffer,
                                       uint32_t zero_padding) const;

  // 写入存储系统
  [[nodiscard]] bool WriteToStorage(uint64_t set_id,
                                    const std::string& block_id,
                                    const std::vector<uint8_t>& buffer,
                                    uint32_t replica,
                                    uint32_t ogn,
                                    uint32_t algorithm,
                                    std::string& error_message);

  // 计算 zero_padding（如果存储系统要求对齐）
  [[nodiscard]] uint32_t CalculateZeroPadding(uint64_t block_size) const;
};

}  // namespace us3_turbo::backend
```

---

## 📁 新建文件：`backend/src/put_block_service.cpp`

```cpp
#include "backend/src/put_block_service.h"

#include <cstring>
#include <vector>

#include <brpc/controller.h>
#include <spdlog/spdlog.h>

namespace us3_turbo::backend {

PutBlockServiceImpl::PutBlockServiceImpl() {
  spdlog::info("PutBlockServiceImpl: initialized");
}

PutBlockServiceImpl::~PutBlockServiceImpl() {
  spdlog::info("PutBlockServiceImpl: shutdown");
}

void PutBlockServiceImpl::PutBlock(google::protobuf::RpcController* controller,
                                  const ProxySetPutBlockRequest* request,
                                  ProxySetPutBlockResponse* response,
                                  google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  
  const auto& req_id = request->request_id();
  spdlog::info("PutBlock: req={}, block_id={}, size={}, mode={}",
               req_id, request->block_id(), request->block_size(),
               static_cast<int>(request->source().mode()));

  response->set_ok(false);
  response->set_block_id(request->block_id());

  // 1. 拉取数据
  std::vector<uint8_t> buffer;
  std::string error_message;
  bool pull_ok = false;

  if (request->source().mode() == PULL_MODE_GDS_TOKEN) {
    pull_ok = PullDataGds(request->source(), request->block_size(), buffer, error_message);
  } else if (request->source().mode() == PULL_MODE_UCX_REMOTE_MEMORY) {
    pull_ok = PullDataUcx(request->source(), request->block_size(), buffer, error_message);
  } else {
    error_message = "invalid pull mode: " + std::to_string(request->source().mode());
    spdlog::error("PutBlock: req={}, {}", req_id, error_message);
  }

  if (!pull_ok) {
    response->set_error_code(-1);
    response->set_error_message(error_message);
    spdlog::error("PutBlock: req={}, failed to pull data: {}", req_id, error_message);
    return;
  }

  // 2. 计算 zero_padding（如果存储系统要求对齐）
  const uint32_t zero_padding = CalculateZeroPadding(request->block_size());
  if (zero_padding > 0) {
    buffer.resize(buffer.size() + zero_padding, 0);  // 填充零字节
  }

  // 3. 计算 CRC32C
  const uint32_t crc32c = ComputeCrc32c(buffer, zero_padding);

  // 4. 校验 CRC（如果 Proxy 传了 expected_crc32c）
  if (request->expected_crc32c() != 0 && crc32c != request->expected_crc32c()) {
    error_message = "CRC mismatch: expected=0x" + 
                    std::to_string(request->expected_crc32c()) +
                    ", actual=0x" + std::to_string(crc32c);
    response->set_error_code(-2);
    response->set_error_message(error_message);
    spdlog::error("PutBlock: req={}, {}", req_id, error_message);
    return;
  }

  // 5. 写入存储系统
  if (!WriteToStorage(request->set_id(), request->block_id(), buffer,
                     request->replica(), request->ogn(), request->algorithm(),
                     error_message)) {
    response->set_error_code(-3);
    response->set_error_message(error_message);
    spdlog::error("PutBlock: req={}, failed to write storage: {}", req_id, error_message);
    return;
  }

  // 6. 返回成功结果
  response->set_ok(true);
  response->set_bytes_written(buffer.size());
  response->set_crc32c(crc32c);
  response->set_zero_padding(zero_padding);

  spdlog::info("PutBlock: req={}, success, bytes={}, crc32c=0x{:08x}, padding={}",
               req_id, buffer.size(), crc32c, zero_padding);
}

bool PutBlockServiceImpl::PullDataGds(const SetPullSource& source,
                                     uint64_t block_size,
                                     std::vector<uint8_t>& buffer,
                                     std::string& error_message) {
  // TODO: 实现 GDS 拉取逻辑
  // 1. 解析 gds_rdma_token
  // 2. 调用 cuFile API 从 GPU 显存拉取数据
  // 3. 从 source.source_offset() 开始读取 block_size 字节
  
  spdlog::warn("PullDataGds: not implemented, token={}, offset={}, size={}",
               source.gds_rdma_token(), source.source_offset(), block_size);
  
  // 模拟数据（用于测试）
  buffer.resize(block_size, 0xAB);
  return true;
}

bool PutBlockServiceImpl::PullDataUcx(const SetPullSource& source,
                                     uint64_t block_size,
                                     std::vector<uint8_t>& buffer,
                                     std::string& error_message) {
  // TODO: 实现 UCX 拉取逻辑
  // 1. 连接到 client_ucx_addr
  // 2. 用 ucp_get_nbx 从 remote_addr + source_offset 拉取 block_size 字节
  // 3. 使用 packed_rkey 进行 RDMA 访问
  
  spdlog::warn("PullDataUcx: not implemented, addr=0x{:x}, offset={}, size={}",
               source.remote_addr(), source.source_offset(), block_size);
  
  // 模拟数据（用于测试）
  buffer.resize(block_size, 0xCD);
  return true;
}

uint32_t PutBlockServiceImpl::ComputeCrc32c(const std::vector<uint8_t>& buffer,
                                           uint32_t zero_padding) const {
  // TODO: 实现真实的 CRC32C 计算（使用硬件加速或库函数）
  // 注意：CRC 包含原始数据 + zero_padding
  
  // 简化实现：返回数据大小的哈希（用于测试）
  return static_cast<uint32_t>(buffer.size() ^ 0x12345678);
}

bool PutBlockServiceImpl::WriteToStorage(uint64_t set_id,
                                        const std::string& block_id,
                                        const std::vector<uint8_t>& buffer,
                                        uint32_t replica,
                                        uint32_t ogn,
                                        uint32_t algorithm,
                                        std::string& error_message) {
  // TODO: 实现存储系统写入逻辑
  // 1. 根据 set_id 找到对应的存储节点
  // 2. 使用 replica/ogn/algorithm 进行纠删码编码
  // 3. 写入存储系统（可能是多副本或纠删码分片）
  
  spdlog::info("WriteToStorage: set_id={}, block_id={}, size={}, replica={}, ogn={}, algo={}",
               set_id, block_id, buffer.size(), replica, ogn, algorithm);
  
  // 模拟写入成功
  return true;
}

uint32_t PutBlockServiceImpl::CalculateZeroPadding(uint64_t block_size) const {
  // 如果存储系统要求块大小对齐（如 4KB），计算需要填充的字节数
  constexpr uint64_t alignment = 4096;
  const uint64_t remainder = block_size % alignment;
  return remainder == 0 ? 0 : static_cast<uint32_t>(alignment - remainder);
}

}  // namespace us3_turbo::backend
```

---

## 📁 新建文件：`backend/src/main.cpp`（Backend 启动入口）

```cpp
#include <brpc/server.h>
#include <gflags/gflags.h>
#include <spdlog/spdlog.h>

#include "backend/src/put_block_service.h"

DEFINE_int32(port, 8080, "Backend service port");
DEFINE_int32(idle_timeout_s, -1, "Connection idle timeout");

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  
  spdlog::set_level(spdlog::level::info);
  spdlog::info("Backend starting on port {}", FLAGS_port);

  brpc::Server server;
  us3_turbo::backend::PutBlockServiceImpl service;

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

  spdlog::info("Backend started, listening on port {}", FLAGS_port);
  server.RunUntilAskedToQuit();

  spdlog::info("Backend shutting down");
  return 0;
}
```

---

## ✅ 验证步骤

1. **编译 Backend**：
   ```bash
   g++ -std=c++17 -I. -I<brpc_include> -I<protobuf_include> \
     backend/src/put_block_service.cpp backend/src/main.cpp \
     -o backend_server \
     -lbrpc -lprotobuf -lgflags -lpthread
   ```

2. **启动 Backend**：
   ```bash
   ./backend_server --port=8080
   ```

3. **测试 RPC 调用**（从 Proxy 端）：
   ```cpp
   BackendRpc backend("127.0.0.1:8080", std::chrono::milliseconds(5000));
   
   ProxySetPutBlockRequest req;
   req.request_id = "test_req";
   req.set_id = 42;
   req.block_id = "obj_123_0";
   req.block_size = 1024;
   req.source = SetPullSource::Gds("token_abc", 0);
   
   ProxySetPutBlockResponse resp;
   bool ok = backend.PutBlock(req, resp);
   // 验证 resp.ok 和 resp.crc32c
   ```

---

## 📝 TODO 项（需要后续实现）

### 高优先级

1. **PullDataGds**：
   - 集成 cuFile API（`cuFileHandleRegister` / `cuFileRead`）
   - 解析 `gds_rdma_token`（可能需要反序列化 CUDA IPC handle）
   - 从 `source_offset` 开始读取 `block_size` 字节

2. **PullDataUcx**：
   - 初始化 UCX context 和 worker
   - 连接到 `client_ucx_addr`
   - 用 `ucp_get_nbx` 从 `remote_addr + source_offset` 拉取数据
   - 使用 `packed_rkey` 进行 RDMA 访问

3. **ComputeCrc32c**：
   - 使用硬件加速（`__builtin_ia32_crc32` 或 `crc32c` 库）
   - 确保包含 zero_padding 的计算

4. **WriteToStorage**：
   - 对接真实存储系统的 API
   - 实现纠删码编码（根据 `ogn` / `algorithm`）
   - 处理多副本写入（根据 `replica`）

### 低优先级

5. **CalculateZeroPadding**：
   - 确认存储系统的对齐要求（当前假设 4KB）
   - 如果不需要对齐，返回 0

6. **错误处理**：
   - 细化错误码（-1 拉取失败，-2 CRC 失败，-3 写入失败）
   - 增加重试逻辑（如网络抖动导致的拉取失败）

---

## 📝 注意事项

1. ✅ Backend 端使用 protobuf 消息，不依赖 Client 的 C++ 结构体
2. ✅ 支持 GDS 和 UCX 两种拉取模式
3. ✅ CRC 校验在 Backend 端完成（端到端校验）
4. ✅ zero_padding 逻辑在 Backend 端实现（Proxy 和 Client 不感知）
5. ⚠️ 当前所有核心逻辑都是 TODO（模拟实现），需要后续对接真实组件
