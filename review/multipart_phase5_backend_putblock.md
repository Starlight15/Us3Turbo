# 阶段 5：实现 Backend PutBlock 服务（支持 offset 拉取）

## 目标

在 Backend 端实现 `PutBlock` RPC 服务，接收 Proxy 发来的带 offset 的 block 请求，使用真实 offset 从 client 拉取数据。

---

## 约束

1. **支持 offset 参数**：修改现有 GDS/UCX sink，用真实 `source_offset` 替换硬编码的 `0`
2. **GDS/UCX 独立处理**：根据 proto 的 `oneof source` 分别调用 GDS 或 UCX 拉取逻辑
3. **返回 block ETag**：计算并返回 block 级别的 ETag（SHA1 + base64）
4. **CRC32C 可选计算**：默认关闭，通过构造函数 flag 控制是否计算 CRC32C
5. **暂时 discard**：本阶段仍 discard 数据（不写入存储），阶段 7 再对接真实存储

---

## Proto 服务定义（已在阶段 1 定义）

```protobuf
service BackendDataPlane {
  rpc PutBlock(ProxyBackendPutBlockRequest) returns (ProxyBackendPutBlockResponse);
}
```

---

## Backend 服务实现

### 文件：`backend/src/backend_data_plane_service.h`

在现有类中新增 `PutBlock` 方法：

```cpp
#pragma once
#include "control_plane.pb.h"
#include "backend/src/backend_gds_sink.h"
#include "backend/src/ucx_sink.h"
#include <brpc/controller.h>

namespace us3_turbo::backend {

class BackendDataPlaneService : public BackendDataPlane {
 public:
  BackendDataPlaneService(bool enable_crc_compute = false);  // 默认关闭 CRC 计算
  
  // ===== 现有单步接口（保留） =====
  void GdsPut(brpc::Controller* cntl,
              const ClientProxyPutRequest* request,
              PutPathResult* response,
              google::protobuf::Closure* done) override;
  
  void UcxPut(brpc::Controller* cntl,
              const ClientProxyPutRequest* request,
              PutPathResult* response,
              google::protobuf::Closure* done) override;
  
  // ===== 新增分段上传接口 =====
  void PutBlock(brpc::Controller* cntl,
                const ProxyBackendPutBlockRequest* request,
                ProxyBackendPutBlockResponse* response,
                google::protobuf::Closure* done) override;
  
 private:
  std::unique_ptr<BackendGdsSink> gds_sink_;
  std::unique_ptr<UcxSink> ucx_sink_;
  bool enable_crc_compute_;  // 是否启用 CRC32C 计算
};

}  // namespace
```

---

## PutBlock 实现逻辑

### 文件：`backend/src/backend_data_plane_service.cpp`

```cpp
void BackendDataPlaneService::PutBlock(
    brpc::Controller* cntl,
    const ProxyBackendPutBlockRequest* request,
    ProxyBackendPutBlockResponse* response,
    google::protobuf::Closure* done) {
  
  brpc::ClosureGuard done_guard(done);
  
  // 1. 根据 source 类型分发
  if (request->has_gds_source()) {
    HandleGdsBlock(request, response);
  } else if (request->has_ucx_source()) {
    HandleUcxBlock(request, response);
  } else {
    response->set_ok(false);
    response->set_error_message("source not set");
  }
}

// GDS 路径处理
void BackendDataPlaneService::HandleGdsBlock(
    const ProxyBackendPutBlockRequest* request,
    ProxyBackendPutBlockResponse* response) {
  
  const auto& src = request->gds_source();
  
  // 参数校验
  if (src.rdma_token().empty() || src.block_size() == 0) {
    response->set_ok(false);
    response->set_error_message("gds_source incomplete");
    return;
  }
  
  // 构造 object_id（用于日志和后续存储）
  std::string object_id = request->upload_id() + "/" + std::to_string(request->part_number());
  
  // 调用 GDS sink 拉取数据
  void* data = nullptr;
  bool ok = gds_sink_->ReceiveBlock(
      object_id,
      src.rdma_token(),
      src.block_size(),
      src.source_offset(),  // ← 真实 offset，不再是 0
      &data);
  
  if (!ok) {
    response->set_ok(false);
    response->set_error_message("gds pull failed");
    return;
  }
  
  // 计算 block etag（必须，用于对象 etag 计算）
  std::string block_etag = gds_sink_->ComputeBlockETag(data, src.block_size());
  response->set_etag(block_etag);
  
  // CRC 计算（可选，默认关闭）
  if (enable_crc_compute_) {
    uint32_t crc = gds_sink_->ComputeCrc32c(data, src.block_size());
    response->set_crc32c(crc);
  }
  
  response->set_ok(true);
  
  LOG(INFO) << "PutBlock GDS: upload_id=" << request->upload_id()
            << ", part_number=" << request->part_number()
            << ", block_no=" << request->block_no()
            << ", offset=" << src.source_offset()
            << ", size=" << src.block_size()
            << ", etag=" << block_etag;
}

// UCX 路径处理
void BackendDataPlaneService::HandleUcxBlock(
    const ProxyBackendPutBlockRequest* request,
    ProxyBackendPutBlockResponse* response) {
  
  const auto& src = request->ucx_source();
  
  // 参数校验
  if (src.remote_addr() == 0 || src.packed_rkey().empty() ||
      src.client_ucx_addr().empty() || src.block_size() == 0) {
    response->set_ok(false);
    response->set_error_message("ucx_source incomplete");
    return;
  }
  
  std::string object_id = request->upload_id() + "/" + std::to_string(request->part_number());
  
  // 调用 UCX sink 拉取数据（remote_addr 已包含偏移）
  void* data = nullptr;
  bool ok = ucx_sink_->ReceiveBlock(
      object_id,
      src.remote_addr(),       // 已加偏移的地址
      src.packed_rkey(),
      src.client_ucx_addr(),
      src.block_size(),
      &data);
  
  if (!ok) {
    response->set_ok(false);
    response->set_error_message("ucx pull failed");
    return;
  }
  
  // 计算 block etag
  std::string block_etag = ucx_sink_->ComputeBlockETag(data, src.block_size());
  response->set_etag(block_etag);
  
  // CRC 计算（可选）
  if (enable_crc_compute_) {
    uint32_t crc = ucx_sink_->ComputeCrc32c(data, src.block_size());
    response->set_crc32c(crc);
  }
  
  response->set_ok(true);
  
  LOG(INFO) << "PutBlock UCX: upload_id=" << request->upload_id()
            << ", part_number=" << request->part_number()
            << ", block_no=" << request->block_no()
            << ", remote_addr=" << std::hex << src.remote_addr()
            << ", size=" << src.block_size()
            << ", etag=" << block_etag;
}
```

---

## 修改 GDS Sink（支持 offset）

### 文件：`backend/src/backend_gds_sink.h`

修改函数签名，增加 `source_offset` 参数：

```cpp
class BackendGdsSink {
 public:
  // 现有方法（保留）
  bool ReceiveAndDiscard(
      const std::string& object_id,
      const std::string& rdma_token,
      uint64_t length);
  
  // 新增方法（拉取数据，返回数据指针）
  bool ReceiveBlock(
      const std::string& object_id,
      const std::string& rdma_token,
      uint64_t length,
      uint64_t source_offset,  // ← 新增参数
      void** out_data);
  
  // 计算 block ETag（SHA1 + base64）
  std::string ComputeBlockETag(const void* data, size_t length);
  
  // 计算 CRC32C（可选，外部调用）
  uint32_t ComputeCrc32c(const void* data, size_t length);
  
 private:
  // cuObjServer 实例
  cuObjServer* server_;
};
```

### 文件：`backend/src/backend_gds_sink.cpp`

```cpp
bool BackendGdsSink::ReceiveBlock(
    const std::string& object_id,
    const std::string& rdma_token,
    uint64_t length,
    uint64_t source_offset,
    void** out_data) {
  
  // 1. 校验长度（cuObjServer 限制 1GB）
  if (length > 1ULL * 1024 * 1024 * 1024) {
    LOG(ERROR) << "block size exceeds 1GB: " << length;
    return false;
  }
  
  // 2. 从 pool 获取 pinned buffer
  auto lease = buffer_pool_.Acquire(length);
  if (!lease.valid()) {
    LOG(ERROR) << "failed to acquire buffer";
    return false;
  }
  
  // 3. 解析 rdma_token 获取 remote_buf_start
  uint64_t remote_buf_start = ParseTokenAddress(rdma_token);
  
  // 4. 调用 cuObjServer handlePutObject（关键：用真实 source_offset）
  cuObjStatus status;
  int ret = server_->handlePutObject(
      object_id.c_str(),
      lease.mr(),                // 本地 memory region
      remote_buf_start,          // 远端基地址（从 token 解析）
      length,                    // 本次拉取大小
      rdma_token.c_str(),
      channel_,
      source_offset,             // ← 远端偏移（之前硬编码为 0）
      &status,
      nullptr);
  
  if (ret != 0 || status != CUOBJ_SUCCESS) {
    LOG(ERROR) << "handlePutObject failed: ret=" << ret
               << ", status=" << static_cast<int>(status);
    return false;
  }
  
  // 5. 返回数据指针
  *out_data = lease.data();
  
  LOG(INFO) << "GDS block received: object_id=" << object_id
            << ", offset=" << source_offset
            << ", length=" << length;
  
  return true;
}

// 计算 block ETag（SHA1 + base64）
std::string BackendGdsSink::ComputeBlockETag(const void* data, size_t length) {
  // 使用 SHA1 计算 hash
  std::string sha1 = utils::SHA1(static_cast<const uint8_t*>(data), length);
  
  // Base64 编码
  return utils::Base64Encode(sha1);
}

// CRC32C 计算（可选，外部调用）
uint32_t BackendGdsSink::ComputeCrc32c(const void* data, size_t length) {
  // 简化版：调用系统库或手写 CRC32C
  // 生产环境建议使用 SSE4.2 的 _mm_crc32_u64 指令
  uint32_t crc = 0;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    crc = crc32c_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  }
  return crc;
}
```

---

## 修改 UCX Sink（支持 offset）

### 文件：`backend/src/ucx_sink.h`

```cpp
class UcxSink {
 public:
  // 现有方法（保留）
  bool ReceiveAndDiscard(
      const std::string& object_id,
      uint64_t remote_addr,
      const std::string& packed_rkey,
      const std::string& client_ucx_addr,
      uint64_t length);
  
  // 新增方法（拉取数据，返回数据指针）
  bool ReceiveBlock(
      const std::string& object_id,
      uint64_t remote_addr,     // 已加偏移的地址
      const std::string& packed_rkey,
      const std::string& client_ucx_addr,
      uint64_t length,
      void** out_data);
  
  // 计算 block ETag（SHA1 + base64）
  std::string ComputeBlockETag(const void* data, size_t length);
  
  // 计算 CRC32C（可选，外部调用）
  uint32_t ComputeCrc32c(const void* data, size_t length);
  
 private:
  ucp_worker_h worker_;
  std::mutex worker_mu_;
};
```

### 文件：`backend/src/ucx_sink.cpp`

```cpp
bool UcxSink::ReceiveBlock(
    const std::string& object_id,
    uint64_t remote_addr,
    const std::string& packed_rkey,
    const std::string& client_ucx_addr,
    uint64_t length,
    void** out_data) {
  
  std::lock_guard lock(worker_mu_);  // UCX worker 非线程安全
  
  // 1. 分配本地 buffer
  std::vector<uint8_t> buffer(length);
  
  // 2. 解析 client_ucx_addr 并建立 endpoint
  ucp_ep_h ep = nullptr;
  if (!DialEndpoint(client_ucx_addr, ep)) {
    LOG(ERROR) << "failed to dial endpoint: " << client_ucx_addr;
    return false;
  }
  
  // 3. Unpack rkey
  ucp_rkey_h rkey_h = nullptr;
  ucs_status_t status = ucp_ep_rkey_unpack(ep, packed_rkey.data(), &rkey_h);
  if (status != UCS_OK) {
    LOG(ERROR) << "ucp_ep_rkey_unpack failed: " << ucs_status_string(status);
    ucp_ep_destroy(ep);
    return false;
  }
  
  // 4. 执行 RDMA GET（拉取数据）
  //    关键：remote_addr 已经是加偏移后的地址（proxy 计算好的）
  ucp_request_param_t param;
  param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK;
  param.cb.send = [](void* req, ucs_status_t st, void* user_data) {
    *(ucs_status_t*)user_data = st;
  };
  
  ucs_status_t get_status = UCS_INPROGRESS;
  void* req = ucp_get_nbx(ep, buffer.data(), length, remote_addr, rkey_h, &param);
  
  if (UCS_PTR_IS_ERR(req)) {
    LOG(ERROR) << "ucp_get_nbx failed";
    ucp_rkey_destroy(rkey_h);
    ucp_ep_destroy(ep);
    return false;
  }
  
  // 5. 等待完成
  if (req != nullptr) {
    while (get_status == UCS_INPROGRESS) {
      ucp_worker_progress(worker_);
    }
    ucp_request_free(req);
  }
  
  ucp_rkey_destroy(rkey_h);
  ucp_ep_destroy(ep);
  
  if (get_status != UCS_OK) {
    LOG(ERROR) << "ucp_get failed: " << ucs_status_string(get_status);
    return false;
  }
  
  // 6. 返回数据指针
  *out_data = buffer.data();
  
  LOG(INFO) << "UCX block received: object_id=" << object_id
            << ", remote_addr=" << std::hex << remote_addr
            << ", length=" << length;
  
  return true;
}

// 计算 block ETag（SHA1 + base64）
std::string UcxSink::ComputeBlockETag(const void* data, size_t length) {
  std::string sha1 = utils::SHA1(static_cast<const uint8_t*>(data), length);
  return utils::Base64Encode(sha1);
}

// CRC32C 计算（可选，外部调用）
uint32_t UcxSink::ComputeCrc32c(const void* data, size_t length) {
  uint32_t crc = 0;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    crc = crc32c_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  }
  return crc;
}
```

---

## 编译验证

```bash
# 1. 编译 Backend 服务
cd backend
g++ -std=c++17 -I../proto -I../generated -I/usr/local/cuda/include \
    -c src/backend_data_plane_service.cpp \
    -o build/backend_service.o

# 2. 编译 GDS/UCX sink
g++ -std=c++17 -I../proto -I/usr/local/cuda/include \
    -c src/backend_gds_sink.cpp -o build/gds_sink.o

g++ -std=c++17 -I../proto -I/usr/local/ucx/include \
    -c src/ucx_sink.cpp -o build/ucx_sink.o

# 3. 链接
g++ build/backend_service.o build/gds_sink.o build/ucx_sink.o \
    ../generated/control_plane.pb.o \
    -lbrpc -lprotobuf -lcuda -lucx -lpthread \
    -o build/backend_server
```

---

## 单元测试用例

```cpp
// 文件：backend/test/test_gds_sink.cpp

TEST(BackendGdsSink, PullWithOffset) {
  BackendGdsSink sink;
  
  // Mock: client 已注册 20MB buffer，取 token
  std::string token = "mock-token-20MB";
  
  // 拉取 offset=8MB, size=4MB 的数据
  uint32_t crc32c;
  bool ok = sink.ReceiveAndComputeCrc(
      "test-object",
      token,
      4 * 1024 * 1024,      // 4MB
      8 * 1024 * 1024,      // offset=8MB
      crc32c);
  
  ASSERT_TRUE(ok);
  EXPECT_NE(crc32c, 0);  // 应该有真实 CRC 值
}
```

---

## 验收标准

- [ ] `PutBlock` RPC 能正确分发 GDS/UCX 请求
- [ ] GDS sink 的 `handlePutObject` 调用使用真实 `source_offset`（非 0）
- [ ] UCX sink 的 `ucp_get_nbx` 使用加偏移后的 `remote_addr`
- [ ] 对 4MB block 计算的 CRC32C 非零且可复现
- [ ] 日志输出包含 upload_id、part_number、block_no、offset、crc32c

---

## 后续阶段依赖

- **阶段 6**：Client 端分段注册与调用（完整端到端测试）
- **阶段 7**：对接真实存储（替换 discard 逻辑为 WriteToStorage）
