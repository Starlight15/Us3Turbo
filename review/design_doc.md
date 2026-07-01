# Us3Turbo 高性能对象存储系统设计方案

## 文档信息

- **版本**：v1.0
- **日期**：2026-07-01
- **阶段**：一期（实现写入）
- **状态**：设计中

---

## 一、背景与目标

### 1.1 业务背景

对象存储可作为 AI 训练和推理场景的数据湖。传统对象存储基于 TCP/IP 网络栈，在高性能场景下存在以下瓶颈：

1. **网络延迟高**：TCP 协议栈开销（内核态切换、拷贝、协议处理）
2. **吞吐量受限**：单流带宽难以充分利用高速网络（100Gbps+）
3. **CPU 开销大**：网络协议栈消耗大量 CPU 资源

### 1.2 技术目标

**Us3Turbo** 通过以下技术提升对象存储性能：

- **GDS（GPU Direct Storage）**：GPU 显存直接到存储，绕过 CPU 和主机内存
- **RDMA（UCX）**：用户态网络栈，零拷贝、内核旁路
- **Pull 模式**：存储节点主动拉取数据，减少客户端逻辑复杂度

### 1.3 性能目标（一期）

| 指标 | 目标 | 说明 |
|------|------|------|
| **单对象写入延迟** | < 10ms | 1MB 对象，GDS 路径 |
| **聚合带宽** | > 80 Gbps | 单客户端，多并发流 |
| **CPU 利用率** | < 20% | 存储节点 CPU 开销 |
| **扩展性** | 线性扩展 | 支持 100+ 存储节点 |

---

## 二、系统架构

### 2.1 整体架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                          Client 层                               │
│  ┌──────────────┐                    ┌──────────────┐           │
│  │ GPU 应用     │                    │ CPU 应用     │           │
│  │ (训练/推理)  │                    │ (预处理)     │           │
│  └──────┬───────┘                    └──────┬───────┘           │
│         │ GPU Memory                        │ Host Memory       │
│         ▼                                   ▼                   │
│  ┌──────────────┐                    ┌──────────────┐           │
│  │ GDS Manager  │                    │ UCX Manager  │           │
│  │ (cuFile)     │                    │ (RDMA)       │           │
│  └──────┬───────┘                    └──────┬───────┘           │
│         │                                   │                   │
│         └───────────────┬───────────────────┘                   │
│                         │ brpc (控制面)                          │
│                         ▼                                       │
└─────────────────────────────────────────────────────────────────┘
                          │
                          │ RPC: GdsPut / UcxPut
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│                          Proxy 层                                │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ ProxyService                                               │ │
│  │  - 接收 Client 请求                                         │ │
│  │  - 选择 Set（负载均衡）                                      │ │
│  │  - 生成 object_id                                          │ │
│  │  - 对象分块（16MB chunks）                                  │ │
│  │  - 索引落盘                                                 │ │
│  └────────────┬───────────────────────────────────────────────┘ │
│               │                                                 │
│               │ 并发 RPC: PutBlock × N                           │
│               ▼                                                 │
└─────────────────────────────────────────────────────────────────┘
                          │
                          │ 多个 PutBlock RPC（每个 chunk 一个）
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Backend 层 (ufile-ac)                       │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ PutBlockService                                            │ │
│  │  1. 解析 source（GDS token / UCX remote memory）           │ │
│  │  2. 主动拉取数据（Pull 模式）：                             │ │
│  │     - GDS: cuFileRead(token) 从 GPU 显存拉取                │ │
│  │     - UCX: ucp_get(remote_addr) 从 Host 内存拉取            │ │
│  │  3. 计算 CRC32C（硬件加速）                                 │ │
│  │  4. 纠删码编码（replica=3 或 EC 2+1）                       │ │
│  │  5. 写入存储系统（Set）                                     │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                 │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐             │
│  │  Set 1      │  │  Set 2      │  │  Set N      │             │
│  │  (3 副本)   │  │  (EC 2+1)   │  │  ...        │             │
│  └─────────────┘  └─────────────┘  └─────────────┘             │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 关键设计决策

#### 2.2.1 Pull 模式 vs Push 模式

| 维度 | Pull 模式（采用） | Push 模式 |
|------|------------------|----------|
| **客户端逻辑** | 简单（只需提供 token） | 复杂（需要推送数据） |
| **流控** | Backend 控制（避免过载） | Client 控制（易过载） |
| **错误处理** | Backend 重试拉取 | Client 需要重试推送 |
| **RDMA 适配性** | 天然适配（Backend 发起 GET） | 需要双向连接 |

**决策**：采用 Pull 模式，Backend 主动拉取数据。

#### 2.2.2 分块策略

- **Chunk 大小**：16MB（一期固定，二期可配置）
- **分块位置**：Proxy 层（Client 无需感知）
- **并发度**：所有 chunk 并发上传到不同 Backend 节点

**优势**：
- 提高并发度（100MB 对象 = 7 个并发流）
- 负载均衡（chunk 分散到不同 Backend）
- 容错性（单个 chunk 失败可重试）

#### 2.2.3 数据路径隔离

- **GDS 路径**：GPU 显存 → GDS → Backend（零 CPU 拷贝）
- **UCX 路径**：Host 内存 → RDMA → Backend（零内核拷贝）
- **代码隔离**：两条路径完全独立（便于 GDS 机器稀缺时跳过测试）

---

## 三、模块设计

### 3.1 Client 模块

#### 3.1.1 功能职责

1. **统一 API**：提供 `PutObject()` 接口，支持 GDS / UCX / ALL 三种模式
2. **内存管理**：
   - GDS：注册 GPU 显存，生成 RDMA token
   - UCX：注册 Host 内存，获取 remote_addr 和 rkey
3. **请求构造**：填充 `ClientProxyPutRequest`（bucket/key/size/path/source）
4. **RPC 调用**：通过 brpc 调用 Proxy 的 `GdsPut()` 或 `UcxPut()`
5. **CRC 校验**：计算本地 CRC，与 Proxy 返回的 CRC 对比

#### 3.1.2 核心流程（GDS 路径）

```cpp
// 1. 注册 GPU buffer
client.RegisterDeviceBuffer(gpu_ptr, size);

// 2. 获取 GDS token
std::string token = gds_manager_->AcquireToken(gpu_ptr, size);

// 3. 构造请求
ClientProxyPutRequest req;
req.bucket = "my-bucket";
req.key = "my-key";
req.object_size = size;
req.path = PutDataPath::kGds;
req.gds_source = GdsDataSource{.rdma_token = token};

// 4. 调用 Proxy
ClientProxyPutResponse resp;
client.PutObject(req, resp);

// 5. 校验结果
if (resp.gds_result && resp.gds_result->ok) {
  // 验证 CRC（可选）
  if (local_crc != resp.gds_result->crc32c) {
    // CRC 不匹配，报错
  }
}
```

#### 3.1.3 核心数据结构

见《阶段1：定义数据结构》文档中的 `ClientProxyPutRequest` / `ClientProxyPutResponse`。

---

### 3.2 Proxy 模块

#### 3.2.1 功能职责

1. **请求接收**：接收 Client 的 `GdsPut()` / `UcxPut()` RPC
2. **Set 选择**：根据 bucket/key 哈希选择存储 set_id（负载均衡）
3. **对象 ID 生成**：生成全局唯一的 object_id（UUID 或时间戳）
4. **对象分块**：
   - 将对象拆分为多个 16MB chunk
   - 计算每个 chunk 的 `source_offset` 和 `object_offset`
5. **并发转发**：
   - 为每个 chunk 生成 `ProxySetPutBlockRequest`
   - 并发调用 Backend 的 `PutBlock()` RPC
6. **结果汇总**：
   - 收集所有 chunk 的上传结果
   - 计算对象级别的 etag 和 CRC
7. **索引落盘**：将 object_id → bucket/key 映射写入索引数据库
8. **重试逻辑**：chunk 上传失败时重试（最多 3 次）

#### 3.2.2 核心流程

```
Client Request (10MB 对象)
         ↓
  ┌──────────────┐
  │ 选择 Set     │ → set_id = hash(bucket + key) % 100
  └──────┬───────┘
         ↓
  ┌──────────────┐
  │ 生成对象 ID  │ → object_id = "obj_" + timestamp
  └──────┬───────┘
         ↓
  ┌──────────────┐
  │ 分块         │ → [Chunk0: 0-16MB] (注：10MB < 16MB，只有1个chunk)
  └──────┬───────┘
         ↓
  ┌──────────────┐
  │ 并发转发     │ → Backend.PutBlock(chunk0)
  └──────┬───────┘
         ↓
  ┌──────────────┐
  │ 汇总结果     │ → etag, crc32c, bytes_written
  └──────┬───────┘
         ↓
  ┌──────────────┐
  │ 索引落盘     │ → DB.Insert(object_id → bucket/key)
  └──────┬───────┘
         ↓
   返回 Client
```

#### 3.2.3 分块算法

```cpp
const uint64_t CHUNK_SIZE = 16 * 1024 * 1024;  // 16MB
const uint64_t num_chunks = (object_size + CHUNK_SIZE - 1) / CHUNK_SIZE;

for (uint64_t i = 0; i < num_chunks; ++i) {
  ProxySetPutBlockRequest block;
  block.block_no = i;
  block.object_offset = i * CHUNK_SIZE;
  block.block_size = std::min(CHUNK_SIZE, object_size - i * CHUNK_SIZE);
  block.last_piece = (i == num_chunks - 1);
  
  // GDS 路径
  block.source = SetPullSource::Gds(gds_token, i * CHUNK_SIZE);
  
  // 或 UCX 路径
  // block.source = SetPullSource::Ucx(remote_addr, rkey, ucx_addr, i * CHUNK_SIZE);
}
```

#### 3.2.4 重试策略

- **最大重试次数**：3 次（包含首次尝试）
- **退避策略**：指数退避 + 随机抖动
  - 第 1 次重试：100ms ± 50%
  - 第 2 次重试：200ms ± 50%
  - 第 3 次重试：400ms ± 50%
- **失败判定**：所有重试均失败后，标记该 chunk 失败

#### 3.2.5 索引结构

```sql
CREATE TABLE object_index (
  object_id VARCHAR(64) PRIMARY KEY,
  bucket VARCHAR(256) NOT NULL,
  object_key VARCHAR(1024) NOT NULL,
  set_id BIGINT NOT NULL,
  object_size BIGINT NOT NULL,
  etag VARCHAR(64) NOT NULL,
  crc32c INT UNSIGNED NOT NULL,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_bucket_key (bucket, object_key)
);
```

---

### 3.3 Backend 模块 (ufile-ac)

#### 3.3.1 功能职责

1. **RPC 服务**：提供 `PutBlock()` RPC 接口
2. **数据拉取**：
   - **GDS 路径**：解析 `gds_rdma_token`，调用 `cuFileRead()` 从 GPU 显存拉取
   - **UCX 路径**：连接 `client_ucx_addr`，调用 `ucp_get_nbx()` 从 Host 内存拉取
3. **数据校验**：
   - 计算拉取数据的 CRC32C
   - 与 Proxy 传入的 `expected_crc32c` 对比（如果非 0）
4. **纠删码编码**：
   - 根据 `replica` / `ogn` / `algorithm` 参数进行编码
   - 副本模式：复制 3 份到不同磁盘
   - EC 模式：2+1 纠删码（2 个数据块 + 1 个校验块）
5. **存储写入**：
   - 将编码后的数据写入 Set 对应的存储节点
   - 多副本/多分片并发写入
6. **对齐填充**：
   - 如果存储系统要求块大小对齐（如 4KB），填充 zero_padding
   - 返回填充字节数给 Proxy（用于 CRC 校验）

#### 3.3.2 核心流程（GDS 路径）

```
PutBlock RPC 请求
         ↓
  ┌──────────────┐
  │ 解析 token   │ → gds_rdma_token（CUDA IPC handle）
  └──────┬───────┘
         ↓
  ┌──────────────┐
  │ cuFileRead   │ → 从 GPU 显存 offset=source_offset 读取 block_size 字节
  └──────┬───────┘  （绕过 CPU，直接 DMA 到 Backend 内存）
         ↓
  ┌──────────────┐
  │ CRC32C 计算  │ → crc = crc32c(data + zero_padding)
  └──────┬───────┘
         ↓
  ┌──────────────┐
  │ CRC 校验     │ → if (expected_crc != 0 && crc != expected_crc) { error }
  └──────┬───────┘
         ↓
  ┌──────────────┐
  │ 纠删码编码   │ → EC(2+1): data → [shard0, shard1, parity]
  └──────┬───────┘
         ↓
  ┌──────────────┐
  │ 并发写入     │ → shard0 → disk1, shard1 → disk2, parity → disk3
  └──────┬───────┘
         ↓
   返回 Proxy
   (ok=true, crc32c, bytes_written, zero_padding)
```

#### 3.3.3 GDS 拉取实现要点

```cpp
bool PutBlockServiceImpl::PullDataGds(const SetPullSource& source,
                                     uint64_t block_size,
                                     std::vector<uint8_t>& buffer) {
  // 1. 解析 gds_rdma_token（假设是序列化的 cudaIpcMemHandle_t）
  cudaIpcMemHandle_t ipc_handle;
  memcpy(&ipc_handle, source.gds_rdma_token().data(), sizeof(ipc_handle));
  
  // 2. 打开远程 GPU 内存
  void* remote_gpu_ptr;
  cudaIpcOpenMemHandle(&remote_gpu_ptr, ipc_handle, cudaIpcMemLazyEnablePeerAccess);
  
  // 3. 使用 cuFile 读取（DMA 到 Host 内存，无 CPU 参与）
  CUfileHandle_t cf_handle;
  cuFileHandleRegister(&cf_handle, remote_gpu_ptr, block_size);
  
  buffer.resize(block_size);
  ssize_t ret = cuFileRead(cf_handle, buffer.data(), block_size, 
                          source.source_offset(), 0);
  
  // 4. 清理
  cuFileHandleDeregister(cf_handle);
  cudaIpcCloseMemHandle(remote_gpu_ptr);
  
  return (ret == block_size);
}
```

#### 3.3.4 UCX 拉取实现要点

```cpp
bool PutBlockServiceImpl::PullDataUcx(const SetPullSource& source,
                                     uint64_t block_size,
                                     std::vector<uint8_t>& buffer) {
  // 1. 连接到 Client 的 UCX worker
  ucp_ep_h endpoint;
  ConnectToClient(source.client_ucx_addr(), &endpoint);
  
  // 2. 解包 rkey
  ucp_rkey_h rkey;
  ucp_ep_rkey_unpack(endpoint, source.packed_rkey().data(), &rkey);
  
  // 3. RDMA GET（从 Client 远程内存拉取）
  buffer.resize(block_size);
  ucp_request_param_t params = {};
  void* req = ucp_get_nbx(endpoint, buffer.data(), block_size,
                         source.remote_addr() + source.source_offset(),
                         rkey, &params);
  
  // 4. 等待完成
  WaitForUcxRequest(req);
  
  // 5. 清理
  ucp_rkey_destroy(rkey);
  return true;
}
```

#### 3.3.5 存储写入策略

**副本模式（replica=3）**：
```
原始数据 (16MB)
    ↓
┌─────────┬─────────┬─────────┐
│ Disk 1  │ Disk 2  │ Disk 3  │
│ (副本1) │ (副本2) │ (副本3) │
└─────────┴─────────┴─────────┘
   16MB      16MB      16MB
```

**纠删码模式（EC 2+1）**：
```
原始数据 (16MB)
    ↓
 [编码器]
    ↓
┌─────────┬─────────┬─────────┐
│ Disk 1  │ Disk 2  │ Disk 3  │
│ (数据块1)│ (数据块2)│ (校验块)│
└─────────┴─────────┴─────────┘
   8MB       8MB       8MB
```

---

## 四、数据结构设计

### 4.1 Client → Proxy 接口

#### 4.1.1 请求结构

```cpp
enum class PutDataPath : uint8_t {
  kNone = 0,  // 无效值，Client 必须显式指定
  kGds = 1,   // GDS 路径
  kUcx = 2,   // UCX 路径
  kAll = 3,   // 同时走两条路径（容错）
};

struct GdsDataSource {
  std::string rdma_token;  // CUDA GDS RDMA token
};

struct UcxDataSource {
  uint64_t remote_addr;      // UCX 远程内存地址
  std::string packed_rkey;   // UCX packed rkey
  std::string client_ucx_addr;  // Client UCX 地址 "ip:port"
};

struct ClientProxyPutRequest {
  std::string request_id;
  std::string bucket;
  std::string key;
  uint64_t object_size;
  PutDataPath path;  // 必须显式指定
  std::optional<GdsDataSource> gds_source;
  std::optional<UcxDataSource> ucx_source;
};
```

#### 4.1.2 响应结构

```cpp
struct PutPathResult {
  bool ok;
  int32_t error_code;
  std::string error_message;
  std::string etag;
  uint32_t crc32c;
  uint64_t bytes_written;
};

struct ClientProxyPutResponse {
  std::string object_id;
  std::optional<PutPathResult> gds_result;  // GDS 路径结果
  std::optional<PutPathResult> ucx_result;  // UCX 路径结果
};
```

### 4.2 Proxy → Backend 接口

#### 4.2.1 请求结构

```cpp
enum class SetPullMode : uint8_t {
  kGdsToken = 1,        // Backend 用 GDS token 拉取
  kUcxRemoteMemory = 2, // Backend 用 UCX 拉取
};

struct SetPullSource {
  SetPullMode mode;
  
  // GDS 专用字段
  std::string gds_rdma_token;
  
  // UCX 专用字段
  uint64_t remote_addr;
  std::string packed_rkey;
  std::string client_ucx_addr;
  
  // 公共字段：当前 chunk 在 Client buffer 中的偏移
  uint64_t source_offset;
};

struct ProxySetPutBlockRequest {
  std::string request_id;  // 格式：client_req_id + "_chunk" + block_no
  
  // 存储策略
  uint64_t set_id;      // Proxy 选中的 set
  
  // 对象元数据
  std::string object_id;
  uint64_t object_offset;  // 当前 chunk 在对象中的偏移
  
  // Chunk 元数据
  std::string block_id;    // object_id + "_" + block_no
  uint32_t block_no;
  uint64_t block_size;
  bool last_piece;
  
  // 存储协议参数
  uint32_t replica;    // 副本数（3 或 EC 参数）
  uint32_t ogn;        // 纠删码原始块数
  uint32_t algorithm;  // 编码算法
  
  // 数据源
  SetPullSource source;
  
  // 端到端校验
  uint32_t expected_crc32c;
};
```

#### 4.2.2 响应结构

```cpp
struct ProxySetPutBlockResponse {
  bool ok;
  int32_t error_code;
  std::string error_message;
  
  std::string block_id;
  uint64_t bytes_written;
  uint32_t crc32c;
  uint32_t zero_padding;  // 对齐填充的零字节数
};
```

---

## 五、时序设计

### 5.1 GDS 路径完整时序图

```
Client (GPU App)    Client (GDS Mgr)    Proxy              Backend (ufile-ac)
    |                     |                 |                        |
    | 1. 申请 GPU 内存    |                 |                        |
    |-------------------->|                 |                        |
    |   cudaMalloc        |                 |                        |
    |<--------------------|                 |                        |
    |   gpu_ptr           |                 |                        |
    |                     |                 |                        |
    | 2. 注册 GDS         |                 |                        |
    |-------------------->|                 |                        |
    |   RegisterBuffer    |                 |                        |
    |                     | cuMemGetRDMAToken                        |
    |<--------------------|                 |                        |
    |   rdma_token        |                 |                        |
    |                     |                 |                        |
    | 3. 发起上传（10MB） |                 |                        |
    |-------------------->|                 |                        |
    |   PutObject(        |                 |                        |
    |     token,          |                 |                        |
    |     size=10MB)      |                 |                        |
    |                     |                 |                        |
    |                     | 4. RPC: GdsPut  |                        |
    |                     |---------------->|                        |
    |                     |                 |                        |
    |                     |                 | 5. 选择 set_id=42      |
    |                     |                 | 6. 生成 object_id      |
    |                     |                 | 7. 分块（10MB < 16MB） |
    |                     |                 |    → 1 个 chunk        |
    |                     |                 |                        |
    |                     |                 | 8. RPC: PutBlock       |
    |                     |                 |   (chunk0, size=10MB)  |
    |                     |                 |----------------------->|
    |                     |                 |                        |
    |                     |                 |                        | 9. 解析 token
    |                     |                 |                        | 10. cuFileRead
    |                     |<=========================================>|    (GPU→Backend)
    |                     |                 |      RDMA 拉取 10MB    |
    |                     |                 |                        |
    |                     |                 |                        | 11. 计算 CRC32C
    |                     |                 |                        | 12. 纠删码 (EC 2+1)
    |                     |                 |                        | 13. 写入存储
    |                     |                 |                        |
    |                     |                 |   Response: ok=true    |
    |                     |                 |   crc32c, bytes=10MB   |
    |                     |                 |<-----------------------|
    |                     |                 |                        |
    |                     |                 | 14. 汇总结果           |
    |                     |                 | 15. 索引落盘           |
    |                     |                 |                        |
    |                     | Response: etag, |                        |
    |                     |   crc32c        |                        |
    |                     |<----------------|                        |
    |                     |                 |                        |
    |   ok, etag, crc     |                 |                        |
    |<--------------------|                 |                        |
    |                     |                 |                        |
    | 16. 验证 CRC（可选）|                 |                        |
    |                     |                 |                        |
```

### 5.2 多 Chunk 并发时序（100MB 对象）

```
Client              Proxy                Backend1          Backend2
  |                   |                      |                 |
  |--PutObject(100MB)->|                      |                 |
  |                   |                      |                 |
  |                   | Split: 7 chunks      |                 |
  |                   | (16MB × 6 + 4MB)     |                 |
  |                   |                      |                 |
  |                   |--Chunk0(16MB)------->|                 |
  |                   |--Chunk1(16MB)------->|                 |
  |                   |--Chunk2(16MB)---------------------->|   |
  |                   |--Chunk3(16MB)---------------------->|   |
  |                   |--Chunk4(16MB)------->|                 |
  |                   |--Chunk5(16MB)---------------------->|   |
  |                   |--Chunk6(4MB)-------->|                 |
  |                   |                      |                 |
  |                   |                      | Pull & Write    |
  |                   |                      |<======Client    |
  |                   |<--Chunk0 OK----------|                 |
  |                   |                      |                 |
  |                   |                      | Pull & Write    |
  |                   |<--Chunk1 OK----------|                 |
  |                   |                      |                 |
  |                   |                      |   Pull & Write  |
  |                   |                      |   <======Client |
  |                   |<--Chunk2 OK----------------------|     |
  |                   |                      |                 |
  |                   | ... (并发完成所有chunk)               |
  |                   |                      |                 |
  |<--Response--------|                      |                 |
  |   etag, crc       |                      |                 |
```

### 5.3 ALL 模式时序（GDS + UCX 同时上传）

```
Client           Proxy (GDS)    Proxy (UCX)    Backend1 (GDS)  Backend2 (UCX)
  |                  |               |                |               |
  |--PutObject------>|               |                |               |
  | (path=kAll)      |               |                |               |
  |                  |               |                |               |
  | Client 并发调用: |               |                |               |
  |----------------->|               |                |               |
  | GdsPut(token)    |               |                |               |
  |                  |--Chunk0(GDS)----------------->|               |
  |                  |               |                |               |
  |----------------------------->|                    |               |
  | UcxPut(remote_addr)          |                    |               |
  |                  |           |--Chunk0(UCX)--------------------->|
  |                  |               |                |               |
  |                  |               |                | Pull (GDS)    |
  |                  |               |                |<====Client    |
  |                  |               |                |               |
  |                  |               |                |   Pull (UCX)  |
  |                  |               |                |   <====Client |
  |                  |               |                |               |
  |<--GDS Result-----|               |                |               |
  |   ok, etag1      |               |                |               |
  |                  |               |                |               |
  |<--UCX Result-----------------------------|        |               |
  |   ok, etag2      |               |                |               |
  |                  |               |                |               |
  | 任意一条成功即可 |               |                |               |
```

---

## 六、性能分析

### 6.1 延迟分解（GDS 路径，1MB 对象）

| 阶段 | 耗时 | 说明 |
|------|------|------|
| Client → Proxy RPC | 0.5ms | brpc 本地网络延迟 |
| Proxy 分块逻辑 | 0.1ms | 1MB < 16MB，无需分块 |
| Proxy → Backend RPC | 0.5ms | brpc 本地网络延迟 |
| Backend GDS 拉取 | 2ms | GPU → Backend RDMA 传输（1MB @ 4GB/s） |
| Backend CRC 计算 | 0.3ms | 硬件加速 CRC32C |
| Backend 纠删码编码 | 1ms | EC 2+1 编码 |
| Backend 存储写入 | 3ms | SSD 写入延迟 |
| Backend → Proxy 响应 | 0.5ms | brpc 返回 |
| Proxy → Client 响应 | 0.5ms | brpc 返回 |
| **总延迟** | **8.4ms** | **< 10ms 目标** ✅ |

### 6.2 吞吐量分析（单 Client，100 并发流）

```
假设：
- 对象大小：10MB
- Chunk 大小：16MB（10MB < 16MB，1 个 chunk）
- 并发度：100 个对象同时上传
- 网络带宽：100 Gbps
- 单对象延迟：8.4ms

吞吐量 = 对象大小 × 并发度 / 延迟
       = 10MB × 100 / 8.4ms
       = 1000MB / 8.4ms
       ≈ 119 GB/s
       ≈ 95 Gbps

结论：单 Client 可达 95 Gbps，接近 100 Gbps 网络上限 ✅
```

### 6.3 CPU 利用率分析

| 模块 | CPU 开销 | 说明 |
|------|---------|------|
| Client | < 5% | GDS 零拷贝，无 CPU 参与数据传输 |
| Proxy | < 10% | 只处理元数据（分块、RPC 转发） |
| Backend (GDS 拉取) | < 5% | cuFile DMA，CPU 不参与 |
| Backend (CRC) | < 3% | 硬件加速 CRC32C |
| Backend (纠删码) | < 5% | SIMD 优化的 EC 编码 |
| Backend (存储写入) | < 5% | DMA 写入 SSD |
| **总计** | **< 20%** | **满足目标** ✅ |

---

## 七、容错与高可用

### 7.1 故障场景与处理

| 故障类型 | 检测方式 | 处理策略 | 恢复时间 |
|---------|---------|---------|---------|
| **Client 崩溃** | Proxy RPC 超时 | 释放资源，返回错误 | 立即 |
| **Proxy 崩溃** | Client RPC 超时 | Client 重试到其他 Proxy | < 1s |
| **Backend 崩溃** | Proxy RPC 超时 | 重试到其他 Backend | < 1s |
| **网络抖动** | RPC 超时 | 指数退避重试（3 次） | < 3s |
| **GDS 拉取失败** | cuFile 错误码 | 降级到 CPU 拷贝（二期）| N/A |
| **存储写入失败** | 磁盘错误 | 写入备用磁盘 | < 5s |

### 7.2 数据一致性保证

1. **端到端 CRC 校验**：
   - Client 计算 CRC → Proxy 传递 → Backend 验证
   - 不匹配则拒绝写入，返回错误

2. **幂等性**：
   - 使用 `request_id` 去重
   - 重复请求返回缓存结果

3. **原子性**：
   - 所有 chunk 写入成功后才落盘索引
   - 部分失败则清理已写入的 chunk

### 7.3 降级策略

| 场景 | 降级方案 |
|------|---------|
| GDS 不可用 | 自动切换到 UCX 路径 |
| UCX 不可用 | 自动切换到 GDS 路径 |
| Backend 负载过高 | 限流（返回 429） |
| 存储空间不足 | 返回 507（二期支持配额） |

---

## 八、监控与可观测性

### 8.1 核心指标

#### 8.1.1 性能指标

| 指标 | 说明 | 告警阈值 |
|------|------|---------|
| `put_latency_p99` | P99 上传延迟 | > 50ms |
| `put_latency_p999` | P999 上传延迟 | > 100ms |
| `put_throughput` | 聚合吞吐量（GB/s） | < 50 GB/s |
| `chunk_pull_latency` | Backend 拉取延迟 | > 10ms |
| `storage_write_latency` | 存储写入延迟 | > 20ms |

#### 8.1.2 可靠性指标

| 指标 | 说明 | 告警阈值 |
|------|------|---------|
| `put_success_rate` | 上传成功率 | < 99.9% |
| `crc_mismatch_rate` | CRC 校验失败率 | > 0.01% |
| `retry_rate` | 重试率 | > 5% |
| `backend_error_rate` | Backend 错误率 | > 1% |

#### 8.1.3 资源指标

| 指标 | 说明 | 告警阈值 |
|------|------|---------|
| `cpu_usage` | CPU 利用率 | > 80% |
| `memory_usage` | 内存使用率 | > 85% |
| `network_bandwidth` | 网络带宽使用 | > 90% |
| `disk_iops` | 磁盘 IOPS | > 90% 容量 |

### 8.2 日志设计

#### 8.2.1 日志级别

- **INFO**：正常请求日志（采样 1%）
- **WARN**：重试、降级、慢请求（> 50ms）
- **ERROR**：失败请求、CRC 不匹配、存储错误

#### 8.2.2 日志格式（结构化 JSON）

```json
{
  "timestamp": "2026-07-01T10:00:00.123Z",
  "level": "INFO",
  "module": "proxy",
  "request_id": "req_abc123",
  "bucket": "my-bucket",
  "key": "my-key",
  "object_size": 10485760,
  "path": "gds",
  "set_id": 42,
  "num_chunks": 1,
  "latency_ms": 8.4,
  "status": "success"
}
```

### 8.3 链路追踪

使用 `request_id` 贯穿全链路：

```
Client: req_abc123
  ↓
Proxy: req_abc123
  ↓
Backend: req_abc123_chunk0_gds
```

---

## 九、实施计划

### 9.1 一期目标（当前）

**目标**：实现基础写入功能，验证技术可行性。

| 里程碑 | 交付物 | 时间 |
|-------|--------|------|
| M1：数据结构定义 | C++ 结构体、Protobuf 定义 | Week 1 |
| M2：Proxy 实现 | 分块逻辑、RPC 转发 | Week 2-3 |
| M3：Backend 实现 | GDS/UCX 拉取、存储写入 | Week 3-4 |
| M4：集成测试 | 端到端测试、性能验证 | Week 5 |
| M5：文档完善 | 设计文档、运维手册 | Week 6 |

### 9.2 二期规划

1. **读取功能**：实现 GetObject（Range 读取、分块并发读）
2. **性能优化**：
   - 零拷贝优化（sendfile、splice）
   - 内存池（减少分配开销）
   - 批量索引落盘（减少数据库 QPS）
3. **功能增强**：
   - 多版本支持（对象覆盖）
   - 生命周期管理（自动删除过期对象）
   - 配额管理（bucket 级别限流）
4. **运维增强**：
   - 动态配置（chunk 大小、重试次数）
   - 监控面板（Grafana）
   - 自动化测试（压测工具）

---

## 十、风险与挑战

### 10.1 技术风险

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| GDS 稳定性 | 高 | 降级到 UCX；充分测试 |
| UCX 兼容性 | 中 | 多网卡测试；版本锁定 |
| 纠删码性能 | 中 | SIMD 优化；硬件加速 |
| 存储系统对接 | 高 | Mock 接口；分阶段对接 |

### 10.2 工程挑战

| 挑战 | 影响 | 应对策略 |
|------|------|---------|
| GDS 机器稀缺 | 测试困难 | 代码隔离；模拟测试 |
| 多模块协同 | 联调复杂 | 接口先行；Mock 测试 |
| 性能调优 | 耗时长 | 压测工具；监控完善 |
| 文档维护 | 可维护性 | 自动生成；持续更新 |

---

## 十一、附录

### 11.1 术语表

| 术语 | 全称 | 说明 |
|------|------|------|
| GDS | GPU Direct Storage | NVIDIA 技术，GPU 显存直接访问存储 |
| UCX | Unified Communication X | 统一通信框架，支持 RDMA |
| RDMA | Remote Direct Memory Access | 远程直接内存访问 |
| EC | Erasure Coding | 纠删码 |
| Set | Storage Set | 存储集合（一组存储节点） |
| Chunk | Data Chunk | 数据块（16MB） |
| CRC32C | Cyclic Redundancy Check | 循环冗余校验（Castagnoli 多项式） |

### 11.2 参考文档

1. [NVIDIA GDS 官方文档](https://docs.nvidia.com/gpudirect-storage/)
2. [UCX 官方文档](https://openucx.org/)
3. [brpc 官方文档](https://github.com/apache/brpc)
4. [阶段1：定义数据结构](./proxy_backend_phase1_structures.md)
5. [阶段2：定义 Protobuf](./proxy_backend_phase2_protobuf.md)
6. [阶段3-6：实现文档](./README.md)

---

**文档结束**

