# 分段上传实现总览

## 📋 实施阶段概览

本文档描述了 Us3Turbo 分段上传（Multipart Upload）的完整实施方案，分为 7 个可增量实现的阶段。

---

## 🎯 核心设计决策

基于技术分析和项目约束，采用以下设计：

| 维度 | 决策 | 理由 |
|------|------|------|
| **会话状态存储** | Proxy 内存态（`std::unordered_map`） | 原型阶段快速验证，避免引入外部依赖 |
| **注册粒度** | 分段注册（每个 part 独立注册） | 避免 GDS 1GB 注册上限，支持流式上传 |
| **切分归属** | Proxy 切分（Client 只管 part 级） | 保持 Client 薄，Backend 块大小变化无需改 Client |
| **接口设计** | 保留单步 `GdsPut/UcxPut`，新增分段接口 | 小对象快路径，大对象分段路径，互不影响 |

---

## 📐 架构设计

### 数据流向

```
Client (分段注册)                  Proxy (内存态会话 + 切分)        Backend (按 offset 拉取)
  │                                   │                              │
  ├─ CreateMultipartUpload ────────→ 生成 upload_id,存会话 ───────→ (无操作)
  │                                   sessions_[upload_id] = {...}
  │
  ├─ AcquireToken(part1_ptr, 5MB) ──→ (Client 本地)
  ├─ UploadPartGds(upload_id, ──────→ SplitToBlocks(5MB, 4MB) ─────→ PutBlock(offset=0, 4MB)
  │    part=1, token1, 5MB)           ├─ block1: offset=0, 4M  ───→ PutBlock(offset=4MB, 1MB)
  │                                   └─ block2: offset=4M, 1M      (并发拉取)
  │                                   汇总 CRC/etag,存 part 元数据
  │
  ├─ ReleaseToken(token1) ───────────→ (Client 本地)
  │
  ├─ AcquireToken(part2_ptr, 5MB) ──→ ...
  ├─ UploadPartGds(upload_id, part=2, ...)
  │
  └─ CompleteMultipartUpload ────────→ 校验 part 列表/etag/size ───→ (无操作,或触发 merge)
                                        生成最终 object_id
                                        清理 sessions_[upload_id]
```

### 关键技术点

1. **GDS offset 支持**：`cuMemObjGetRDMAToken(ptr, size, offset, ...)` + `handlePutObject(..., offset, ...)` 底层 API 已具备能力
2. **UCX offset 支持**：`ucp_get_nbx(ep, local_buf, size, remote_addr + offset, ...)` 通过地址偏移实现
3. **两级切分**：
   - Level-1（S3 part）：Client 定义，5MB~5GB
   - Level-2（backend block）：Proxy 切分，默认 4MB
4. **会话元数据**：`UploadSession{upload_id, bucket, key, path, parts[]}`，TTL 3 天

---

## 🗂️ 阶段划分

### ✅ 阶段 1：扩展 Proto 定义

**文件**：`review/multipart_phase1_proto.md`

**目标**：在 `control_plane.proto` 中新增分段上传的 RPC 和消息定义。

**产物**：
- `CreateMultipartUploadRequest/Response`
- `UploadPartGdsRequest` / `UploadPartUcxRequest` / `UploadPartResponse`
- `CompleteMultipartUploadRequest/Response`
- `ProxyBackendPutBlockRequest/Response`（带 `source_offset` / `block_size`）
- `service Control` 新增 4 个 RPC
- `service BackendDataPlane` 新增 `PutBlock` RPC

**依赖**：无

**验证**：protobuf 编译通过，生成的消息类包含 offset 字段

---

### ✅ 阶段 2：实现 Proxy 内存态会话管理

**文件**：`review/multipart_phase2_session_manager.md`

**目标**：实现 `SessionManager` 管理分段上传会话。

**产物**：
- `proxy/src/multipart/upload_session.h`：`UploadSession` / `PartMetadata` 结构
- `proxy/src/multipart/session_manager.h/cpp`：
  - `CreateSession()` 生成 upload_id
  - `AddPart()` 追加 part 元数据
  - `CompleteSession()` 校验 part 列表、生成最终 etag
  - `CleanupExpiredSessions()` TTL 清理
- `proxy/src/common/utils.h`：UUID / SHA1 / Base64 工具函数

**依赖**：阶段 1

**验证**：单元测试会话增删改查、part 去重、连续性校验

---

### ✅ 阶段 3：实现 Proxy 分段上传 RPC 服务

**文件**：`review/multipart_phase3_proxy_rpc.md`

**目标**：在 Proxy 端实现三个 RPC 方法，集成 `SessionManager`。

**产物**：
- `proxy/src/service/proxy_control_plane_service.h/cpp`：
  - `CreateMultipartUpload()` 创建会话
  - `UploadPartGds()` / `UploadPartUcx()` 添加 part（本阶段返回 mock 数据）
  - `CompleteMultipartUpload()` 完成会话

**依赖**：阶段 1、阶段 2

**验证**：brpc_cli 测试三个 RPC，会话状态正确流转

---

### ✅ 阶段 4：实现 Proxy 分段切分与并发上传逻辑

**文件**：`review/multipart_phase4_proxy_split.md`

**目标**：实现 `MultipartPutHandler` 将 part 切分为 block 并发上传到 Backend。

**产物**：
- `proxy/src/multipart/multipart_put_handler.h/cpp`：
  - `SplitToBlocksGds/Ucx()` 算术切分
  - `ExecuteBlocksGds/Ucx()` 并发调用（`std::async`）
  - `CallBackendPutBlockGds/Ucx()` 构造带 offset 的 RPC 请求
  - `AggregateResults()` 汇总 CRC/etag
- 修改阶段 3 的 `UploadPartGds/Ucx`，调用 handler 处理

**依赖**：阶段 1、阶段 2、阶段 3

**验证**：20MB part 切分为 5 个 4MB block，并发调用成功

---

### ✅ 阶段 5：实现 Backend PutBlock 服务（支持 offset 拉取）

**文件**：`review/multipart_phase5_backend_putblock.md`

**目标**：Backend 接收带 offset 的 block 请求，使用真实 offset 拉取数据。

**产物**：
- `backend/src/backend_data_plane_service.h/cpp`：
  - `PutBlock()` RPC 处理（根据 GDS/UCX source 分发）
  - `HandleGdsBlock()` / `HandleUcxBlock()`
- `backend/src/backend_gds_sink.h/cpp`：
  - `ReceiveAndComputeCrc()` 新方法，传入 `source_offset`
  - 修改 `handlePutObject` 调用，用真实 offset 替换 `0`
- `backend/src/ucx_sink.h/cpp`：
  - `ReceiveAndComputeCrc()` 新方法
  - `ucp_get_nbx` 使用 `remote_addr + offset`

**依赖**：阶段 1

**验证**：Backend 能从 offset=8MB 处拉取 4MB 数据，CRC 计算正确

---

### ✅ 阶段 6：实现 Client 端分段注册与调用

**文件**：`review/multipart_phase6_client_impl.md`

**目标**：Client 端实现三个接口，支持分段注册 token/descriptor。

**产物**：
- `client/include/us3_turbo/client/client.h`：
  - `CreateMultipartUpload()`
  - `UploadPartGds()` / `UploadPartUcx()`（每次注册当前 part）
  - `CompleteMultipartUpload()`
- `client/src/rpc/proxy_rpc.h/cpp`：封装 RPC 调用
- 关键逻辑：
  - `AcquireToken(part_ptr, part_size, 0)` 只注册本 part
  - 上传完成后 `ReleaseToken()`
  - 可选本地 CRC 计算与校验

**依赖**：阶段 1、阶段 3-5

**验证**：Client 上传 3 个 5MB part，端到端成功

---

### ✅ 阶段 7：端到端集成测试与性能验证

**文件**：`review/multipart_phase7_integration.md`

**目标**：完整链路测试、性能基准、文档更新。

**产物**：
- `client/test/integration/test_e2e_multipart.cpp`：
  - GDS 路径 20MB / 100MB 测试
  - UCX 路径 12MB 测试
  - 错误场景测试（非连续 part）
- `client/test/benchmark/bench_multipart.cpp`：单步 vs 分段性能对比
- 更新 `review/README.md`：测试覆盖表、性能基准、已知限制

**依赖**：阶段 1-6 全部完成

**验证**：
- 100MB 对象上传成功，吞吐量 > 100 MB/s
- 日志 request_id 贯穿三层
- 错误场景正确拒绝

---

## 📊 依赖关系图

```
阶段1：Proto 定义
   │
   ├──> 阶段2：Proxy 会话管理
   │       │
   │       └──> 阶段3：Proxy RPC 服务
   │               │
   │               └──> 阶段4：Proxy 切分逻辑
   │                       │
   │                       └──> 阶段6：Client 实现
   │                               │
   │                               └──> 阶段7：集成测试
   │
   └──> 阶段5：Backend PutBlock
           │
           └──> 阶段6：Client 实现
                   │
                   └──> 阶段7：集成测试
```

---

## 🚀 实施建议

### 推荐顺序

1. **阶段 1 → 阶段 2 → 阶段 3**：先实现控制面（3-4 天）
2. **阶段 4 + 阶段 5**：Proxy 切分和 Backend 拉取可并行（3-4 天）
3. **阶段 6**：Client 端实现（2-3 天）
4. **阶段 7**：集成测试和性能验证（2 天）

总工期：**约 2 周**

### 并行开发

- **Proxy 团队**：阶段 1 → 2 → 3 → 4
- **Backend 团队**：阶段 1 → 5
- **Client 团队**：阶段 1 → 6（依赖阶段 3-5 完成后联调）

---

## ✅ 完成标志

当以下所有条件满足时，分段上传功能完成：

- ✅ Client 可通过 Proxy 分段上传对象（GDS 路径）
- ✅ Client 可通过 Proxy 分段上传对象（UCX 路径）
- ✅ Backend 可按 offset 从 Client 拉取数据块
- ✅ 100MB 对象上传成功，吞吐量 > 100 MB/s
- ✅ 端到端日志追踪完整（request_id 贯穿）
- ✅ 错误场景（非连续 part）正确拒绝

---

## 📝 后续优化方向（v2）

### 高优先级

1. **会话持久化**：接入 Redis/MongoDB，支持 Proxy 重启恢复
2. **存储对接**：Backend 写入真实存储系统（替换 discard）
3. **AbortMultipartUpload**：实现主动取消接口
4. **ListParts**：查询已上传 part 列表

### 中优先级

5. **Client 并发上传**：多 part 并发上传（当前串行）
6. **重试策略**：单 part 失败自动重试（指数退避）
7. **监控指标**：Prometheus metrics（延迟、吞吐、错误率）

### 低优先级

8. **UploadPartCopy**：支持 server-side copy
9. **分段 Get**：对称实现分段下载
10. **压缩传输**：可选的数据压缩

---

## 📚 文档索引

1. [阶段 1：扩展 Proto 定义](./multipart_phase1_proto.md)
2. [阶段 2：实现 Proxy 内存态会话管理](./multipart_phase2_session_manager.md)
3. [阶段 3：实现 Proxy 分段上传 RPC 服务](./multipart_phase3_proxy_rpc.md)
4. [阶段 4：实现 Proxy 分段切分与并发上传逻辑](./multipart_phase4_proxy_split.md)
5. [阶段 5：实现 Backend PutBlock 服务](./multipart_phase5_backend_putblock.md)
6. [阶段 6：实现 Client 端分段注册与调用](./multipart_phase6_client_impl.md)
7. [阶段 7：端到端集成测试与性能验证](./multipart_phase7_integration.md)

---

## 🔧 技术栈

- **C++17**：std::optional, std::async, std::shared_mutex
- **brpc**：RPC 框架
- **protobuf3**：消息序列化
- **CUDA**：cuMemObjGetRDMAToken, cuFile API
- **UCX**：ucp_get_nbx, ucp_rkey_pack/unpack
- **gtest**：单元测试
- **benchmark**：性能测试

---

## ⚠️ 已知限制（v1）

| 限制 | 影响 | 计划解决版本 |
|------|------|-------------|
| 会话状态仅存 Proxy 内存 | Proxy 重启丢失上传进度 | v2（持久化） |
| Backend discard 模式 | 未真正写入存储 | v2（存储对接） |
| 无 AbortMultipartUpload | 无法主动取消，需等 TTL | v2 |
| 无 ListParts | 无法查询已上传 part | v2 |
| Client 串行上传 | 多 part 场景慢 | v2（并发上传） |
| 单 part 失败不重试 | 需 Client 手动重试 | v2（重试策略） |
