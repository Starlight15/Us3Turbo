# RDMA 分段上传实现提示词

## 架构理解

**核心事实：Backend (ufile-ac) 对分段上传零感知。** Multipart 是纯 proxy 端通过 `IUploadIndex` 索引层实现的抽象：
- `CreateMultipartUpload` → 分配 UUID + obj_id，写 upload 会话到索引
- `UploadPart{Gds,Ucx}` → 以 `{obj_id}_{part_number-1}` 为 block_key，调用 `PutBlockXxx` 写 **1 个 block**（1 part = 1 block，不再按 4MB 子切分）
- `CompleteMultipartUpload` → 汇总 parts → 写 `fileidx_col`（block_size=part_size）→ 清理会话
- `AbortMultipartUpload` → 删除会话（幂等）

Backend 看到的始终是独立的 `PutBlockXxx(key, ...)` 调用，无 multipart 语义。

**RDMA 现有基础设施：**
- `RdmaPutReq::sourceOffset_` 字段已存在，`PutBlockRdma(key, token, source_offset, data_len)` 已就绪
- `RdmaMemoryManager::AcquireDescriptor(ptr, size, out_desc)` 对任意 host buffer 注册 MR + 编码 token
- `EncodeToken(ip, port, rkey, addr, size)` → hex 字符串，token 编码了完整 buffer 区间
- 后台 Accept 线程复用 listener，连接池缓存最多 128 QP

## 目标

为 RDMA 链路实现完整的分段上传支持，对标 GDS 分段上传，使 `CreateMultipartUpload(PATH_RDMA)` → `UploadPartRdma` → `CompleteMultipartUpload` 链路可用。

## 范围与限制

### 纳入范围

1. **Proto 层**：新增 `UploadPartRdmaRequest` + `rpc UploadPartRdma`
2. **Client RPC 层**：`ProxyRpc::UploadPartRdma()`
3. **Client API 层**：
   - `Client::UploadPartRdma(upload_id, part_number, buffer, out_etag, out_error)`
   - `CreateMultipartUpload` 增加 `PATH_RDMA` 支持
4. **Proxy Multipart 层**：
   - `ValidateUploadPartRdma()` — 校验 upload session path == PATH_RDMA + rdma_token 非空
   - `UploadPartRdma()` — 对标 `UploadPartGds`
   - `CreateUpload()` — 增加 `PATH_RDMA` 支持
5. **Proxy Service 层**：`ProxyService::UploadPartRdma` handler（薄委托）
6. **示例程序**：`rdma_put_example.cpp` 增加分段上传 benchmark

### 不纳入范围

- **Backend 协议/代码**：无需修改（`PutBlockRdma` + `RdmaPutReq` + `OSD_RDMA_PUT_REQ/RSP` 已就绪）
- **RDMA GET 分段**：本次只做 PUT 方向
- **分段上传内部子切分**：维持现有 1 part = 1 block 设计，不做 part 内 block 切分
- **RdmaBlockSource proto**：当前 1-part=1-block 模式下 proxy 直接调 `PutBlockRdma`，不走 `BackendDataPlane.PutBlock` RPC，故无需添加 `RdmaBlockSource` 到 `ProxyBackendPutBlockRequest.oneof`

## 实施步骤

### Step 1: Proto 层 (`proto/control_plane.proto`)

新增 `UploadPartRdmaRequest` message 和 RPC：

```protobuf
// 上传单个 part — RDMA (libibverbs) 路径。rdma_token 编码了 client listener
// 地址 + rkey + addr + size，backend 解码后反向 RDMA CM connect →
// ibv_post_send(RDMA_READ)。
message UploadPartRdmaRequest {
  string request_id  = 1;
  string upload_id   = 2;
  uint32 part_number = 3;  // 从 1 开始
  uint64 part_size   = 4;
  string rdma_token  = 5;
}
```

在 `service Control` 中新增：
```protobuf
rpc UploadPartRdma(UploadPartRdmaRequest) returns (UploadPartResponse);
```

### Step 2: Client RPC 层 (`client/src/rpc/proxy_rpc.h` + `.cpp`)

对标 `UploadPartGds`，新增：
```cpp
[[nodiscard]] bool UploadPartRdma(std::string_view req_id, const std::string& upload_id,
                                   std::uint32_t part_number, std::uint64_t part_size,
                                   const std::string& rdma_token, PutPathResult& res) const;
```

实现：构建 `UploadPartRdmaRequest` → `stub()->UploadPartRdma(&cntl, &req, &resp, nullptr)` → 填充 `res` + 失败时 `FailResult`。

### Step 3: Client API 层 (`client/src/client.cpp` + `client.h`)

**3a. `CreateMultipartUpload` 修复**（行 249-250）：
```cpp
// 改为:
const ::us3_turbo::proxy::PutDataPath proto_path =
    (path == PutDataPath::kGds)  ? ::us3_turbo::proxy::PATH_GDS
    : (path == PutDataPath::kRdma) ? ::us3_turbo::proxy::PATH_RDMA
    : ::us3_turbo::proxy::PATH_UCX;
```

**3b. `UploadPartRdma` 新增**（插在 `UploadPartUcx` 之后，`CompleteMultipartUpload` 之前），对标 `UploadPartGds` 模式：
```cpp
bool Client::UploadPartRdma(const std::string& upload_id, std::uint32_t part_number,
                            ConstBufferView buffer, std::string& out_etag,
                            std::string& out_error) const {
  // 1) initialized_ 检查
  // 2) RdmaManager() 非空检查
  // 3) part_size 上限校验
  // 4) AcquireDescriptor(buffer.data, buffer.size, desc)
  // 5) proxy_->UploadPartRdma(req_id, upload_id, part_number, buffer.size, desc.token, res)
  // 6) 可选 CRC 校验 (buffer 是 host 内存，无需 D2H)
  // 7) trace 插桩
}
```

### Step 4: Proxy Multipart 层 (`proxy/src/service/multipart.h` + `.cpp`)

**4a. `CreateUpload` 修复**（行 150）：
```cpp
if (path != PATH_GDS && path != PATH_UCX && path != PATH_RDMA) {
```

**4b. `ValidateUploadPartRdma` 新增**（对标 `ValidateUploadPartGds`）：
```cpp
int Multipart::ValidateUploadPartRdma(const std::string& request_id,
                                       const std::string& upload_id,
                                       std::uint32_t part_number, std::uint64_t part_size,
                                       const std::string& rdma_token, UploadRecord& out_upload) {
  // 1) index_->Get(upload_id, upload) + not found → PROXY_ERR_INVALID_PARAM
  // 2) upload.path != PATH_RDMA → PROXY_ERR_PATH_NOT_SUPPORTED
  // 3) part_number/part_size 校验
  // 4) rdma_token 非空校验
}
```

**4c. `UploadPartRdma` 新增**（对标 `UploadPartGds`）：
```cpp
int Multipart::UploadPartRdma(const std::string& request_id, const std::string& upload_id,
                               std::uint32_t part_number, std::uint64_t part_size,
                               const std::string& rdma_token, UploadPartOutput& out) {
  // 1) ValidateUploadPartRdma
  // 2) block_key = GenerateBlockKey(upload.obj_id, part_number - 1)
  //    file_offset = (part_number - 1) * part_size_limit
  // 3) client_->PutBlockRdma(block_key, rdma_token, /*source_offset=*/0, part_size)
  // 4) WritePartIndex(...) + CleanupWrittenBlocks 回滚
}
```

### Step 5: Proxy Service 层 (`proxy/src/api/proxy_service.h` + `.cpp`)

**5a.** `proxy_service.h` 新增声明：
```cpp
void UploadPartRdma(google::protobuf::RpcController* cntl_base,
                    const UploadPartRdmaRequest* request,
                    UploadPartResponse* response,
                    google::protobuf::Closure* done) override;
```

**5b.** `proxy_service.cpp` 新增实现（对标 `UploadPartGds`）：
```cpp
void ProxyService::UploadPartRdma(...) {
  brpc::ClosureGuard done_guard(done);
  // ... 薄委托到 multipart_->UploadPartRdma(...) + AccessLogger
}
```

### Step 6: 示例程序 (`rtest/examples/gds/rdma_put_example.cpp`)

增加 `--multipart` flag，支持分段上传 benchmark：
- `CreateMultipartUpload(PATH_RDMA)` → `N x UploadPartRdma` → `CompleteMultipartUpload`
- 参数：`--size`（总大小）、`--part-size`（每 part 大小，默认 4MB）、`--concurrency`

## 不变部分（无需修改）

| 组件 | 原因 |
|------|------|
| `RdmaMemoryManager` / `RdmaQp` / `EncodeToken` | 完全复用，单步和分段无区别 |
| `RdmaPutChannel::PutOnce` | 单步上传路径，与分段隔离 |
| `UfileAcClient::PutBlockRdma(key, token, source_offset, data_len)` | Backend 接口已就绪 |
| `RdmaPutReq` / `RdmaPutRsp` 协议编解码 | 已支持 sourceOffset |
| `CompleteMultipartUpload` / `AbortMultipartUpload` | 完全路径无关，无需修改 |
| `WritePartIndex` / `CleanupWrittenBlocks` / `GenerateBlockKey` | 公共辅助方法，无需修改 |
| `ClientProxyPutRequest` / `ClientProxyPutResponse` / `RdmaDataSource` | 本次不改（单步上传用） |
| Backend RDMA 服务端代码 | 无需任何修改 |

## 关键设计决策

1. **1 part = 1 block**：维持 GDS 分段的设计决策。不再 part 内按 4MB 切分多 block 串行写。ufile-ac `PutBlockRdma` 单次支持 ≤16MB。

2. **不走 `BackendDataPlane.PutBlock` RPC**：当前 GDS/UCX 分段也直接调 `PutBlockGds/PutBlockUcx`（通过 TCP 二进制协议），不走 proto 定义的 `BackendDataPlane.PutBlock` RPC 路径。RDMA 保持一致，直接调 `PutBlockRdma`。因此无需新增 `RdmaBlockSource` proto message。

3. **CRC 校验**：RDMA 是 host buffer，无需 D2H，直接 `Crc32c(buffer)` 即可。

4. **token 语义**：每个 part 独立调用 `AcquireDescriptor` 生成 token。token 编码了该 part buffer 的完整区间（addr + size）。backend `PutBlockRdma` 从 token 解码后 `RDMA_READ(addr + source_offset, data_len)` 拉取数据。这里 source_offset=0, data_len=part_size。

## 预期工作量

| 文件 | 改动类型 | 预估行数 |
|------|---------|---------|
| `proto/control_plane.proto` | 新增 message + rpc | +15 |
| `client/src/rpc/proxy_rpc.h` | 新增声明 | +5 |
| `client/src/rpc/proxy_rpc.cpp` | 新增实现 | +25 |
| `client/include/us3_turbo/client/client.h` | 新增声明 | +5 |
| `client/src/client.cpp` | 新增 UploadPartRdma + 修复 CreateMultipartUpload | +55 |
| `proxy/src/service/multipart.h` | 新增声明 | +10 |
| `proxy/src/service/multipart.cpp` | 新增 Validate + UploadPart + 修复 CreateUpload | +70 |
| `proxy/src/api/proxy_service.h` | 新增声明 | +5 |
| `proxy/src/api/proxy_service.cpp` | 新增 handler | +30 |
| **合计** | | **~220 行** |
