# 阶段 3：实现 Proxy 分段上传 RPC 服务

## 目标

在 Proxy 端实现分段上传的三个 RPC 方法：`CreateMultipartUpload`、`UploadPartGds`/`UploadPartUcx`、`CompleteMultipartUpload`，集成阶段 2 的 `SessionManager`。

---

## 约束

1. **与单步接口隔离**：不修改现有 `GdsPut`/`UcxPut` 的实现
2. **GDS/UCX 独立方法**：`UploadPartGds` 和 `UploadPartUcx` 完全独立实现，无共享逻辑
3. **同步调用**：本阶段暂不实现 Backend 切分，UploadPart 只做会话管理和基础校验
4. **依赖阶段 1/2**：使用阶段 1 的 proto 定义和阶段 2 的 `SessionManager`

---

## 文件修改

### 文件：`proxy/src/service/proxy_control_plane_service.h`

在现有类中新增三个方法：

```cpp
#pragma once
#include "control_plane.pb.h"
#include "proxy/src/multipart/session_manager.h"
#include <brpc/controller.h>

namespace us3_turbo::proxy {

class ProxyControlPlaneService : public Control {
 public:
  ProxyControlPlaneService();
  
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
  void CreateMultipartUpload(
      brpc::Controller* cntl,
      const CreateMultipartUploadRequest* request,
      CreateMultipartUploadResponse* response,
      google::protobuf::Closure* done) override;
  
  void UploadPartGds(
      brpc::Controller* cntl,
      const UploadPartGdsRequest* request,
      UploadPartResponse* response,
      google::protobuf::Closure* done) override;
  
  void UploadPartUcx(
      brpc::Controller* cntl,
      const UploadPartUcxRequest* request,
      UploadPartResponse* response,
      google::protobuf::Closure* done) override;
  
  void CompleteMultipartUpload(
      brpc::Controller* cntl,
      const CompleteMultipartUploadRequest* request,
      CompleteMultipartUploadResponse* response,
      google::protobuf::Closure* done) override;
  
 private:
  std::unique_ptr<SessionManager> session_manager_;
  // ... 其他现有成员（如 backend_stub_）
};

}  // namespace
```

---

## 实现逻辑

### 文件：`proxy/src/service/proxy_control_plane_service.cpp`

#### 1. CreateMultipartUpload 实现

```cpp
void ProxyControlPlaneService::CreateMultipartUpload(
    brpc::Controller* cntl,
    const CreateMultipartUploadRequest* request,
    CreateMultipartUploadResponse* response,
    google::protobuf::Closure* done) {
  
  brpc::ClosureGuard done_guard(done);
  
  // 1. 参数校验
  if (request->bucket().empty() || request->key().empty()) {
    response->set_ok(false);
    response->set_error_message("bucket or key is empty");
    return;
  }
  
  if (request->path() != PATH_GDS && request->path() != PATH_UCX) {
    response->set_ok(false);
    response->set_error_message("path must be PATH_GDS or PATH_UCX");
    return;
  }
  
  // 2. 创建会话
  std::string upload_id = session_manager_->CreateSession(
      request->bucket(),
      request->key(),
      request->path());
  
  // 3. 返回响应
  response->set_ok(true);
  response->set_upload_id(upload_id);
  
  LOG(INFO) << "CreateMultipartUpload: upload_id=" << upload_id
            << ", bucket=" << request->bucket()
            << ", key=" << request->key()
            << ", path=" << PutDataPath_Name(request->path());
}
```

#### 2. UploadPartGds 实现

**伪代码逻辑（本阶段暂不调用 Backend）：**

```cpp
void ProxyControlPlaneService::UploadPartGds(
    brpc::Controller* cntl,
    const UploadPartGdsRequest* request,
    UploadPartResponse* response,
    google::protobuf::Closure* done) {
  
  brpc::ClosureGuard done_guard(done);
  
  // 1. 获取会话
  UploadSession* session = session_manager_->GetSession(request->upload_id());
  if (!session) {
    response->set_ok(false);
    response->set_error_message("upload_id not found");
    return;
  }
  
  // 2. 校验路径匹配
  if (session->path != PATH_GDS) {
    response->set_ok(false);
    response->set_error_message("session path is not PATH_GDS");
    return;
  }
  
  // 3. 参数校验
  if (request->part_number() == 0 || request->part_size() == 0) {
    response->set_ok(false);
    response->set_error_message("part_number or part_size is zero");
    return;
  }
  
  if (request->rdma_token().empty()) {
    response->set_ok(false);
    response->set_error_message("rdma_token is empty");
    return;
  }
  
  // 4. TODO（阶段 4）：调用 MultipartPutHandler 切分并上传到 Backend
  //    当前阶段先模拟成功，返回空 etag（阶段 4 会从 Backend 获取真实 etag）
  
  // 5. 构造 PartMetadata 并添加到会话
  PartMetadata part;
  part.part_number = request->part_number();
  part.part_size = request->part_size();
  part.etag = "";  // 阶段 4 会从 Backend 获取
  part.upload_time_ms = GetCurrentTimeMillis();
  
  bool added = session_manager_->AddPart(request->upload_id(), part);
  if (!added) {
    response->set_ok(false);
    response->set_error_message("failed to add part");
    return;
  }
  
  // 6. 返回响应（etag 暂时为空）
  response->set_ok(true);
  response->set_etag("");  // 阶段 4 会填充真实值
  response->set_bytes_written(part.part_size);
  // 注意：不设置 crc32c，因为默认关闭
  
  LOG(INFO) << "UploadPartGds: upload_id=" << request->upload_id()
            << ", part_number=" << request->part_number()
            << ", part_size=" << request->part_size();
}
```

#### 3. UploadPartUcx 实现

**逻辑与 UploadPartGds 对称，替换为 UCX 相关字段：**

```cpp
void ProxyControlPlaneService::UploadPartUcx(
    brpc::Controller* cntl,
    const UploadPartUcxRequest* request,
    UploadPartResponse* response,
    google::protobuf::Closure* done) {
  
  brpc::ClosureGuard done_guard(done);
  
  // 1. 获取会话
  UploadSession* session = session_manager_->GetSession(request->upload_id());
  if (!session) {
    response->set_ok(false);
    response->set_error_message("upload_id not found");
    return;
  }
  
  // 2. 校验路径匹配
  if (session->path != PATH_UCX) {
    response->set_ok(false);
    response->set_error_message("session path is not PATH_UCX");
    return;
  }
  
  // 3. 参数校验
  if (request->part_number() == 0 || request->part_size() == 0) {
    response->set_ok(false);
    response->set_error_message("part_number or part_size is zero");
    return;
  }
  
  if (request->remote_addr() == 0 || request->packed_rkey().empty() ||
      request->client_ucx_addr().empty()) {
    response->set_ok(false);
    response->set_error_message("ucx source fields incomplete");
    return;
  }
  
  // 4. TODO（阶段 4）：调用 MultipartPutHandler 切分并上传到 Backend
  
  // 5. 构造 PartMetadata 并添加到会话
  PartMetadata part;
  part.part_number = request->part_number();
  part.part_size = request->part_size();
  part.etag = "";  // 阶段 4 会从 Backend 获取
  part.upload_time_ms = GetCurrentTimeMillis();
  
  session_manager_->AddPart(request->upload_id(), part);
  
  // 6. 返回响应
  response->set_ok(true);
  response->set_etag("");  // 阶段 4 会填充真实值
  response->set_bytes_written(part.part_size);
  
  LOG(INFO) << "UploadPartUcx: upload_id=" << request->upload_id()
            << ", part_number=" << request->part_number()
            << ", part_size=" << request->part_size();
}
```

#### 4. CompleteMultipartUpload 实现

```cpp
void ProxyControlPlaneService::CompleteMultipartUpload(
    brpc::Controller* cntl,
    const CompleteMultipartUploadRequest* request,
    CompleteMultipartUploadResponse* response,
    google::protobuf::Closure* done) {
  
  brpc::ClosureGuard done_guard(done);
  
  // 1. 调用 SessionManager 完成会话
  std::string object_id, etag, error;
  uint64_t object_size;
  
  std::vector<CompleteMultipartUploadRequest::PartInfo> client_parts;
  for (int i = 0; i < request->parts_size(); ++i) {
    client_parts.push_back(request->parts(i));
  }
  
  bool ok = session_manager_->CompleteSession(
      request->upload_id(),
      client_parts,
      object_id,
      etag,
      object_size,
      error);
  
  if (!ok) {
    response->set_ok(false);
    response->set_error_message(error);
    return;
  }
  
  // 2. 清理会话（成功后立即删除）
  session_manager_->CleanupSession(request->upload_id());
  
  // 3. 返回响应
  response->set_ok(true);
  response->set_object_id(object_id);
  response->set_etag(etag);
  response->set_object_size(object_size);
  
  LOG(INFO) << "CompleteMultipartUpload: upload_id=" << request->upload_id()
            << ", object_id=" << object_id
            << ", object_size=" << object_size;
}
```

---

## 构造函数修改

### 文件：`proxy/src/service/proxy_control_plane_service.cpp`

```cpp
ProxyControlPlaneService::ProxyControlPlaneService()
    : session_manager_(std::make_unique<SessionManager>()) {
  // 启动后台清理线程（可选，阶段 2 的 CleanupExpiredSessions）
  // std::thread([this]() {
  //   while (true) {
  //     std::this_thread::sleep_for(std::chrono::hours(1));
  //     session_manager_->CleanupExpiredSessions(3 * 24 * 3600 * 1000);  // 3天
  //   }
  // }).detach();
}
```

---

## 编译验证

```bash
# 1. 编译 Proxy 服务
cd proxy
g++ -std=c++17 -I../proto -I../generated \
    -c src/service/proxy_control_plane_service.cpp \
    -o build/proxy_service.o

# 2. 链接（假设已有 brpc 库）
g++ build/proxy_service.o build/session_manager.o ../generated/control_plane.pb.o \
    -lbrpc -lprotobuf -lpthread -o build/proxy_server

# 3. 启动 Proxy 并测试（用 brpc_cli 或自定义 client）
./build/proxy_server --port=9090
```

---

## 测试用例

### 手动测试流程（使用 brpc_cli 工具）

```bash
# 1. CreateMultipartUpload
brpc_cli --method=CreateMultipartUpload \
         --input='{"bucket":"test-bucket","key":"test.dat","path":1}' \
         localhost:9090 us3_turbo.proxy.Control

# 预期输出: {"ok":true,"upload_id":"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"}

# 2. UploadPartGds（上传 3 个 part）
brpc_cli --method=UploadPartGds \
         --input='{"upload_id":"<上步的upload_id>","part_number":1,"part_size":5242880,"rdma_token":"mock-token-1"}' \
         localhost:9090 us3_turbo.proxy.Control

brpc_cli --method=UploadPartGds \
         --input='{"upload_id":"<upload_id>","part_number":2,"part_size":5242880,"rdma_token":"mock-token-2"}' \
         localhost:9090 us3_turbo.proxy.Control

brpc_cli --method=UploadPartGds \
         --input='{"upload_id":"<upload_id>","part_number":3,"part_size":1048576,"rdma_token":"mock-token-3"}' \
         localhost:9090 us3_turbo.proxy.Control

# 3. CompleteMultipartUpload
brpc_cli --method=CompleteMultipartUpload \
         --input='{"upload_id":"<upload_id>"}' \
         localhost:9090 us3_turbo.proxy.Control

# 预期输出: {"ok":true,"object_id":"test-bucket/test.dat","etag":"...","object_size":11534336}
```

---

## 验收标准

- [ ] `CreateMultipartUpload` 能返回有效 upload_id
- [ ] `UploadPartGds` 能接受 rdma_token 并返回 mock etag/crc32c
- [ ] `UploadPartUcx` 能接受 ucx source 字段并返回响应
- [ ] 调用 UploadPartGds 时若 session 路径是 PATH_UCX，返回错误
- [ ] `CompleteMultipartUpload` 能校验 part_number 连续性（如跳过 part 2，返回错误）
- [ ] 完成后再次查询该 upload_id，返回 "upload_id not found"

---

## 后续阶段依赖

- **阶段 4**：实现 Proxy 切分逻辑（在 `UploadPartGds`/`UploadPartUcx` 中调用 `MultipartPutHandler`）
- **阶段 5**：Backend PutBlock 实现（接收阶段 4 发来的 block 级请求）
