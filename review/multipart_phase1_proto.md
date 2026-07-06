# 阶段 1：扩展 Proto 定义 — 分段上传接口

## 目标

在现有 `control_plane.proto` 基础上增加分段上传的 RPC 定义和消息结构，保持与现有单步接口（`GdsPut/UcxPut`）隔离。

---

## 约束

1. **保留现有接口**：不修改 `GdsPut/UcxPut` 的定义
2. **GDS/UCX 隔离**：分段上传的 GDS 和 UCX 路径使用独立 RPC 方法（`UploadPartGds` / `UploadPartUcxPut`）
3. **支持 offset 透传**：proto 消息必须包含 `source_offset` / `block_size` 字段，供 backend 拉取子区间
4. **protobuf3 语法**：使用 `optional` 关键字标记可选字段

---

## 新增消息定义

### 文件：`proto/control_plane.proto`

在现有 `service Control` 之前添加以下消息：

```protobuf
// ========== 分段上传控制面消息 ==========

// 1. 初始化分段上传
message CreateMultipartUploadRequest {
  string      request_id = 1;
  string      bucket     = 2;
  string      key        = 3;
  PutDataPath path       = 4;  // PATH_GDS 或 PATH_UCX
}

message CreateMultipartUploadResponse {
  bool   ok            = 1;
  string upload_id     = 2;  // UUID，proxy 生成
  string error_message = 3;
}

// 2. 上传分段 - GDS 路径
message UploadPartGdsRequest {
  string request_id  = 1;
  string upload_id   = 2;
  uint32 part_number = 3;  // 从 1 开始，客户端分配
  uint64 part_size   = 4;  // 本 part 的总大小
  string rdma_token  = 5;  // 本 part 注册的 RDMA token
}

// 3. 上传分段 - UCX 路径
message UploadPartUcxRequest {
  string request_id      = 1;
  string upload_id       = 2;
  uint32 part_number     = 3;
  uint64 part_size       = 4;
  uint64 remote_addr     = 5;  // 本 part 的起始虚拟地址
  bytes  packed_rkey     = 6;  // 本 part 的 packed rkey
  string client_ucx_addr = 7;
}

// 4. 上传分段响应（GDS/UCX 共用）
message UploadPartResponse {
  bool   ok            = 1;
  string etag          = 2;  // 本 part 的 ETag（由 Backend 返回的 block etag 汇总）
  optional uint32 crc32c = 3;  // 可选：本 part 的 CRC32C（仅当 Backend 启用 CRC 计算时返回）
  uint64 bytes_written = 4;
  string error_message = 5;
}

// 5. 完成分段上传
message CompleteMultipartUploadRequest {
  string request_id = 1;
  string upload_id  = 2;
  
  // 可选：客户端提供的 part 列表用于校验
  repeated PartInfo parts = 3;
  
  message PartInfo {
    uint32 part_number = 1;
    string etag        = 2;
  }
}

message CompleteMultipartUploadResponse {
  bool   ok            = 1;
  string object_id     = 2;  // 最终对象标识（bucket/key）
  string etag          = 3;  // 最终对象 ETag
  uint64 object_size   = 4;  // 对象总大小
  string error_message = 5;
}
```

---

## 新增 Proxy → Backend 消息（带 offset）

在 proto 文件末尾新增 Backend 专用消息和服务：

```protobuf
// ========== Proxy → Backend 数据面消息 ==========

// GDS 数据源（带 offset）
message GdsBlockSource {
  string rdma_token    = 1;  // client 提供的 token
  uint64 source_offset = 2;  // 相对 token 注册 region 的偏移（字节）
  uint64 block_size    = 3;  // 本 block 大小
}

// UCX 数据源（带 offset）
message UcxBlockSource {
  uint64 remote_addr     = 1;  // 已加偏移的远端地址
  bytes  packed_rkey     = 2;
  string client_ucx_addr = 3;
  uint64 block_size      = 4;
}

// Proxy 向 Backend 发送的 block 级请求
message ProxyBackendPutBlockRequest {
  string request_id  = 1;
  string upload_id   = 2;
  uint32 part_number = 3;
  uint32 block_no    = 4;  // 该 part 内的 block 序号（从 0 开始）
  
  oneof source {
    GdsBlockSource gds_source = 10;
    UcxBlockSource ucx_source = 11;
  }
}

message ProxyBackendPutBlockResponse {
  bool   ok            = 1;
  string etag          = 2;  // 本 block 的 ETag（SHA1 base64）
  optional uint32 crc32c = 3;  // 可选：本 block 的 CRC32C（仅当 Backend 启用 CRC 计算时返回）
  string error_message = 4;
}

// Backend 数据面服务
service BackendDataPlane {
  rpc PutBlock(ProxyBackendPutBlockRequest) returns (ProxyBackendPutBlockResponse);
}
```

---

## 修改现有 `service Control`

在现有两个 RPC 后添加分段上传方法：

```protobuf
service Control {
  // ===== 现有单步接口（保留不变） =====
  rpc GdsPut(ClientProxyPutRequest) returns (PutPathResult);
  rpc UcxPut(ClientProxyPutRequest) returns (PutPathResult);
  
  // ===== 新增分段上传接口 =====
  rpc CreateMultipartUpload(CreateMultipartUploadRequest) 
      returns (CreateMultipartUploadResponse);
  
  rpc UploadPartGds(UploadPartGdsRequest) 
      returns (UploadPartResponse);
  
  rpc UploadPartUcx(UploadPartUcxRequest) 
      returns (UploadPartResponse);
  
  rpc CompleteMultipartUpload(CompleteMultipartUploadRequest) 
      returns (CompleteMultipartUploadResponse);
}
```

---

## 编译验证

```bash
# 1. 重新生成 C++ 代码
cd proto
protoc --cpp_out=../generated control_plane.proto

# 2. 检查生成的头文件
ls ../generated/control_plane.pb.h
grep "CreateMultipartUploadRequest" ../generated/control_plane.pb.h

# 3. 编译验证（不链接，只检查语法）
g++ -std=c++17 -I../generated -c ../generated/control_plane.pb.cc
```

---

## 验收标准

- [ ] `control_plane.proto` 编译通过，无 protobuf 语法错误
- [ ] 生成的 `control_plane.pb.h` 包含所有新增消息类
- [ ] `Control` service 包含 4 个新 RPC 方法
- [ ] `BackendDataPlane` service 包含 `PutBlock` 方法
- [ ] `GdsBlockSource` / `UcxBlockSource` 包含 `source_offset` / `block_size` 字段

---

## 后续阶段依赖

- **阶段 2**：Proxy 内存态会话管理（依赖本阶段的 `CreateMultipartUploadRequest` 等消息）
- **阶段 3**：Client 分段注册与调用（依赖本阶段的 `UploadPartGdsRequest` 等）
