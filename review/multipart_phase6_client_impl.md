# 阶段 6：实现 Client 端分段注册与调用

## 目标

在 Client 端实现分段上传的三个接口：`CreateMultipartUpload`、`UploadPart`（GDS/UCX 各自独立）、`CompleteMultipartUpload`，支持分段注册 token/descriptor。

---

## 约束

1. **分段注册**：每次 `UploadPart` 只注册当前 part 的 buffer（非整对象）
2. **GDS/UCX 隔离**：两条路径使用独立的 UploadPart 方法（`UploadPartGds` / `UploadPartUcx`）
3. **CRC 校验**：可选地对本地数据计算 CRC32C，与 server 返回值比对
4. **依赖阶段 1-5**：使用阶段 1 的 proto，调用阶段 3-5 实现的 Proxy/Backend 服务

---

## Client API 设计

### 文件：`client/include/us3_turbo/client/client.h`

在现有 `Client` 类中新增方法：

```cpp
#pragma once
#include "control_plane.pb.h"
#include "client/src/common/request.h"
#include <string>
#include <vector>

namespace us3_turbo::client {

class Client {
 public:
  Client(const std::string& proxy_addr, bool enable_crc_verify = false);  // 默认关闭 CRC 校验
  
  // ===== 现有单步上传接口（保留） =====
  bool PutObject(const ClientProxyPutRequest& request,
                 ConstBufferView buffer,
                 ClientProxyPutResponse& response);
  
  // ===== 新增分段上传接口 =====
  
  // 1. 初始化分段上传
  bool CreateMultipartUpload(
      const std::string& bucket,
      const std::string& key,
      PutDataPath path,          // PATH_GDS 或 PATH_UCX
      std::string& out_upload_id,
      std::string& out_error);
  
  // 2. 上传单个 part - GDS 路径
  bool UploadPartGds(
      const std::string& upload_id,
      uint32_t part_number,      // 从 1 开始
      ConstBufferView buffer,    // 本 part 的数据（GPU 或 Host）
      std::string& out_etag,
      std::string& out_error);
  
  // 3. 上传单个 part - UCX 路径
  bool UploadPartUcx(
      const std::string& upload_id,
      uint32_t part_number,
      ConstBufferView buffer,    // 本 part 的数据（Host）
      std::string& out_etag,
      std::string& out_error);
  
  // 4. 完成分段上传
  struct PartInfo {
    uint32_t part_number;
    std::string etag;
  };
  
  bool CompleteMultipartUpload(
      const std::string& upload_id,
      const std::vector<PartInfo>& parts,  // 可选：用于校验
      std::string& out_object_id,
      std::string& out_etag,
      uint64_t& out_size,
      std::string& out_error);
  
 private:
  std::unique_ptr<ProxyRpc> proxy_rpc_;
  std::unique_ptr<GdsMemoryManager> gds_manager_;
  std::unique_ptr<UcxMemoryManager> ucx_manager_;
  bool enable_crc_verify_;  // 是否启用 CRC 校验
};

}  // namespace
```

---

## 实现逻辑

### 文件：`client/src/client/client.cpp`

#### 1. CreateMultipartUpload

```cpp
bool Client::CreateMultipartUpload(
    const std::string& bucket,
    const std::string& key,
    PutDataPath path,
    std::string& out_upload_id,
    std::string& out_error) {
  
  // 1. 构造请求
  CreateMultipartUploadRequest req;
  req.set_request_id(GenRequestId());
  req.set_bucket(bucket);
  req.set_key(key);
  req.set_path(path);
  
  CreateMultipartUploadResponse resp;
  
  // 2. 调用 Proxy RPC
  bool ok = proxy_rpc_->CreateMultipartUpload(req, resp);
  if (!ok || !resp.ok()) {
    out_error = resp.error_message();
    return false;
  }
  
  // 3. 返回 upload_id
  out_upload_id = resp.upload_id();
  
  LOG(INFO) << "CreateMultipartUpload: upload_id=" << out_upload_id
            << ", bucket=" << bucket
            << ", key=" << key
            << ", path=" << PutDataPath_Name(path);
  
  return true;
}
```

#### 2. UploadPartGds（关键：分段注册）

```cpp
bool Client::UploadPartGds(
    const std::string& upload_id,
    uint32_t part_number,
    ConstBufferView buffer,
    std::string& out_etag,
    std::string& out_error) {
  
  // 1. 为本 part 注册 GDS token（关键：每次注册一个 part）
  GdsMemoryManager::Token token;
  bool acquired = gds_manager_->AcquireToken(
      buffer.data,
      buffer.size,
      0,              // offset=0（相对本 part buffer 的偏移）
      token);
  
  if (!acquired) {
    out_error = "failed to acquire GDS token";
    return false;
  }
  
  // 2. 构造请求
  UploadPartGdsRequest req;
  req.set_request_id(GenRequestId());
  req.set_upload_id(upload_id);
  req.set_part_number(part_number);
  req.set_part_size(buffer.size);
  req.set_rdma_token(token.str());
  
  UploadPartResponse resp;
  
  // 3. 调用 Proxy RPC
  bool ok = proxy_rpc_->UploadPartGds(req, resp);
  
  // 4. 释放 token（无论成功失败）
  gds_manager_->ReleaseToken(token);
  
  if (!ok || !resp.ok()) {
    out_error = resp.error_message();
    return false;
  }
  
  // 5. 可选：CRC 校验（仅当 enable_crc_verify_ 为 true 且 server 返回了 CRC）
  if (enable_crc_verify_ && resp.has_crc32c()) {
    uint32_t local_crc = 0;
    if (IsDevicePointer(buffer.data)) {
      // GPU buffer: 需要 D2H 拷贝后计算
      std::vector<uint8_t> host_buf(buffer.size);
      cudaMemcpy(host_buf.data(), buffer.data, buffer.size, cudaMemcpyDeviceToHost);
      local_crc = ComputeCrc32c(host_buf.data(), buffer.size);
    } else {
      // Host buffer: 直接计算
      local_crc = ComputeCrc32c(buffer.data, buffer.size);
    }
    
    if (local_crc != resp.crc32c()) {
      LOG(WARNING) << "CRC mismatch: local=" << local_crc
                   << ", server=" << resp.crc32c();
    }
  }
  
  // 6. 返回结果
  out_etag = resp.etag();
  
  LOG(INFO) << "UploadPartGds: upload_id=" << upload_id
            << ", part_number=" << part_number
            << ", part_size=" << buffer.size
            << ", etag=" << out_etag;
  
  return true;
}
```

#### 3. UploadPartUcx（逻辑对称）

```cpp
bool Client::UploadPartUcx(
    const std::string& upload_id,
    uint32_t part_number,
    ConstBufferView buffer,
    std::string& out_etag,
    std::string& out_error) {
  
  // 1. 为本 part 注册 UCX descriptor
  UcxMemoryManager::Descriptor desc;
  bool acquired = ucx_manager_->AcquireDescriptor(buffer.data, buffer.size, desc);
  
  if (!acquired) {
    out_error = "failed to acquire UCX descriptor";
    return false;
  }
  
  // 2. 构造请求
  UploadPartUcxRequest req;
  req.set_request_id(GenRequestId());
  req.set_upload_id(upload_id);
  req.set_part_number(part_number);
  req.set_part_size(buffer.size);
  req.set_remote_addr(desc.remote_addr);
  req.set_packed_rkey(desc.rkey);
  req.set_client_ucx_addr(desc.client_ucx_addr);
  
  UploadPartResponse resp;
  
  // 3. 调用 Proxy RPC
  bool ok = proxy_rpc_->UploadPartUcx(req, resp);
  
  // 4. 释放 descriptor
  ucx_manager_->ReleaseDescriptor(desc);
  
  if (!ok || !resp.ok()) {
    out_error = resp.error_message();
    return false;
  }
  
  // 5. 可选：CRC 校验（Host buffer 可直接计算）
  if (enable_crc_verify_ && resp.has_crc32c()) {
    uint32_t local_crc = ComputeCrc32c(buffer.data, buffer.size);
    if (local_crc != resp.crc32c()) {
      LOG(WARNING) << "CRC mismatch: local=" << local_crc
                   << ", server=" << resp.crc32c();
    }
  }
  
  // 6. 返回结果
  out_etag = resp.etag();
  
  LOG(INFO) << "UploadPartUcx: upload_id=" << upload_id
            << ", part_number=" << part_number
            << ", part_size=" << buffer.size
            << ", etag=" << out_etag;
  
  return true;
}
```

#### 4. CompleteMultipartUpload

```cpp
bool Client::CompleteMultipartUpload(
    const std::string& upload_id,
    const std::vector<PartInfo>& parts,
    std::string& out_object_id,
    std::string& out_etag,
    uint64_t& out_size,
    std::string& out_error) {
  
  // 1. 构造请求
  CompleteMultipartUploadRequest req;
  req.set_request_id(GenRequestId());
  req.set_upload_id(upload_id);
  
  // 可选：添加 part 列表用于校验
  for (const auto& p : parts) {
    auto* part_info = req.add_parts();
    part_info->set_part_number(p.part_number);
    part_info->set_etag(p.etag);
  }
  
  CompleteMultipartUploadResponse resp;
  
  // 2. 调用 Proxy RPC
  bool ok = proxy_rpc_->CompleteMultipartUpload(req, resp);
  if (!ok || !resp.ok()) {
    out_error = resp.error_message();
    return false;
  }
  
  // 3. 返回结果
  out_object_id = resp.object_id();
  out_etag = resp.etag();
  out_size = resp.object_size();
  
  LOG(INFO) << "CompleteMultipartUpload: upload_id=" << upload_id
            << ", object_id=" << out_object_id
            << ", object_size=" << out_size;
  
  return true;
}
```

---

## Proxy RPC 包装

### 文件：`client/src/rpc/proxy_rpc.h`

```cpp
#pragma once
#include "control_plane.pb.h"
#include <brpc/channel.h>

namespace us3_turbo::client {

class ProxyRpc {
 public:
  explicit ProxyRpc(const std::string& proxy_addr);
  
  // 现有方法（保留）
  bool GdsPut(const ClientProxyPutRequest& req, PutPathResult& resp);
  bool UcxPut(const ClientProxyPutRequest& req, PutPathResult& resp);
  
  // 新增分段上传方法
  bool CreateMultipartUpload(
      const CreateMultipartUploadRequest& req,
      CreateMultipartUploadResponse& resp);
  
  bool UploadPartGds(
      const UploadPartGdsRequest& req,
      UploadPartResponse& resp);
  
  bool UploadPartUcx(
      const UploadPartUcxRequest& req,
      UploadPartResponse& resp);
  
  bool CompleteMultipartUpload(
      const CompleteMultipartUploadRequest& req,
      CompleteMultipartUploadResponse& resp);
  
 private:
  brpc::Channel channel_;
  std::unique_ptr<Control_Stub> stub_;
};

}  // namespace
```

### 文件：`client/src/rpc/proxy_rpc.cpp`

```cpp
bool ProxyRpc::CreateMultipartUpload(
    const CreateMultipartUploadRequest& req,
    CreateMultipartUploadResponse& resp) {
  
  brpc::Controller cntl;
  stub_->CreateMultipartUpload(&cntl, &req, &resp, nullptr);
  
  if (cntl.Failed()) {
    LOG(ERROR) << "RPC failed: " << cntl.ErrorText();
    return false;
  }
  
  return true;
}

bool ProxyRpc::UploadPartGds(
    const UploadPartGdsRequest& req,
    UploadPartResponse& resp) {
  
  brpc::Controller cntl;
  stub_->UploadPartGds(&cntl, &req, &resp, nullptr);
  
  if (cntl.Failed()) {
    LOG(ERROR) << "RPC failed: " << cntl.ErrorText();
    return false;
  }
  
  return true;
}

// UploadPartUcx 和 CompleteMultipartUpload 实现类似
```

---

## 编译验证

```bash
# 1. 编译 Client
cd client
g++ -std=c++17 -I../proto -I../generated -I/usr/local/cuda/include \
    -c src/client/client.cpp -o build/client.o

# 2. 编译 ProxyRpc
g++ -std=c++17 -I../proto -I../generated \
    -c src/rpc/proxy_rpc.cpp -o build/proxy_rpc.o

# 3. 链接
g++ build/client.o build/proxy_rpc.o build/gds_memory_manager.o build/ucx_memory_manager.o \
    ../generated/control_plane.pb.o \
    -lbrpc -lprotobuf -lcuda -lucx -lpthread \
    -o build/us3_turbo_client
```

---

## 端到端测试用例

### 文件：`client/test/test_multipart_upload.cpp`

```cpp
#include "us3_turbo/client/client.h"
#include <gtest/gtest.h>

TEST(MultipartUpload, GdsPath_3Parts_20MB) {
  Client client("localhost:9090");
  
  // 1. 创建会话
  std::string upload_id, error;
  bool ok = client.CreateMultipartUpload("test-bucket", "test.dat", PATH_GDS, upload_id, error);
  ASSERT_TRUE(ok) << error;
  ASSERT_FALSE(upload_id.empty());
  
  // 2. 分配并上传 3 个 part（每个 5MB，最后 5MB）
  const size_t part_size = 5 * 1024 * 1024;
  void* gpu_ptr;
  cudaMalloc(&gpu_ptr, part_size);
  
  std::vector<Client::PartInfo> parts;
  
  for (uint32_t i = 1; i <= 3; ++i) {
    // 填充测试数据（省略）
    ConstBufferView buf{gpu_ptr, part_size};
    
    std::string etag;
    uint32_t crc;
    ok = client.UploadPartGds(upload_id, i, buf, etag, crc, error);
    ASSERT_TRUE(ok) << error;
    
    parts.push_back({i, etag});
  }
  
  cudaFree(gpu_ptr);
  
  // 3. 完成上传
  std::string object_id, etag;
  uint64_t size;
  ok = client.CompleteMultipartUpload(upload_id, parts, object_id, etag, size, error);
  ASSERT_TRUE(ok) << error;
  
  EXPECT_EQ(object_id, "test-bucket/test.dat");
  EXPECT_EQ(size, 15 * 1024 * 1024);  // 3 * 5MB
}

TEST(MultipartUpload, UcxPath_2Parts_10MB) {
  Client client("localhost:9090");
  
  std::string upload_id, error;
  bool ok = client.CreateMultipartUpload("test-bucket", "test.dat", PATH_UCX, upload_id, error);
  ASSERT_TRUE(ok);
  
  const size_t part_size = 5 * 1024 * 1024;
  std::vector<uint8_t> host_buf(part_size);
  
  std::vector<Client::PartInfo> parts;
  
  for (uint32_t i = 1; i <= 2; ++i) {
    ConstBufferView buf{host_buf.data(), part_size};
    
    std::string etag;
    uint32_t crc;
    ok = client.UploadPartUcx(upload_id, i, buf, etag, crc, error);
    ASSERT_TRUE(ok) << error;
    
    parts.push_back({i, etag});
  }
  
  std::string object_id, etag;
  uint64_t size;
  ok = client.CompleteMultipartUpload(upload_id, parts, object_id, etag, size, error);
  ASSERT_TRUE(ok);
  
  EXPECT_EQ(size, 10 * 1024 * 1024);
}
```

---

## 验收标准

- [ ] `CreateMultipartUpload` 能返回有效 upload_id
- [ ] `UploadPartGds` 为每个 part 独立注册 token，成功返回 etag
- [ ] `UploadPartUcx` 为每个 part 独立注册 descriptor，成功返回 etag
- [ ] 上传 3 个 5MB part，`CompleteMultipartUpload` 返回 object_size=15MB
- [ ] 本地计算的 CRC32C 与 server 返回值一致（或打印 warning）
- [ ] 端到端日志贯穿 Client → Proxy → Backend，request_id 可追踪

---

## 后续阶段依赖

- **阶段 7**：集成测试与文档（完整端到端流程 + 性能测试）
