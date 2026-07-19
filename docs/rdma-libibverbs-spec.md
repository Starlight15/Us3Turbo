# CPU Host Memory RDMA via Direct libibverbs — 实现规格书

## 背景与目标

### 问题
当前 UCX 路径 (`ucx_service.cc`, 990 行) 在 CPU host memory 场景下做 4MB RDMA READ 时被协议选择引擎选中 `get/am/bcopy`（AM 软件模拟 GET），导致 8KB 分片、514 次 progress 调用、~80ms 延迟。尝试修改 UCX 源码提高 `rndv/get/zcopy` 优先级无效——因为 UCX 的 client-server 端点不创建 RMA_BW lane，rendezvous 协议根本无法注册。

### 根因
UCX 为 MPI/HPC 消息传递设计，存储场景大块单次 RDMA READ 不适合其多协议协商框架。

### 验证过的正确方案
GDS 路径使用 cuObjServer（封装 `ibv_post_send(RDMA_READ)`），单次 WQE 完成 4MB 传输，无分片，达到 ~3 GiB/s@conc48。

### 目标
用直接 libibverbs 替换 UCX，实现 CPU host memory 的零分片 RDMA READ/WRITE，达到与 GDS 同等的单次大块 DMA 性能。

---

## 约束

### 必须遵守
1. **C++17**，不引入 C++20/23 特性
2. **头文件由调用方自维护**：不依赖 cuObjServer、UCX 或任何第三方 RDMA wrapper 库
3. **仅链接标准系统库**：`libibverbs`（来自 rdma-core，系统自带）、`librdmacm`（如需 CM）
4. **RoCE v2 (RDMA over Converged Ethernet)**：link_layer=Ethernet，active_mtu=1024，GID 索引用于 RoCE
5. **RC (Reliable Connected)** 传输模式：每连接一个 QP，比 DC 更简单标准
6. **单台机器内跨 NIC 场景**：client 使用 `mlx5_0` (192.168.1.x)，backend (ufile-ac) 使用 `mlx5_2` (192.168.1.x)
7. **非阻塞设计**：RDMA READ/WRITE 操作本身不阻塞调用线程，使用 completion polling
8. **支持并发**：多个 QP 可同时进行 RDMA 操作（每个 worker thread 独立 QP）
9. **Buffer 复用**：参考 GDS 的 `PinnedBufferPool`，预分配 + 注册 pinned memory，避免每次分配/注册
10. **错误处理完备**：所有 `ibv_*` 返回值检查，timeout 处理，资源清理
11. **不修改 proxy 协议消息格式（首选）** —— 如果必须修改，最小化改动，保持向后兼容
12. **先替换 PUT 路径（RDMA READ）**，GET 路径后续复用同一基础设施

### 禁止
1. ❌ 不引入新的第三方 C/C++ 库依赖（libibverbs/librdmacm 已是系统库）
2. ❌ 不使用 UCX / libfabric / PMIx
3. ❌ 不引入新的线程模型——复用现有 `workers_` 线程池模式
4. ❌ 不在 client 端启动 listener / 被动 accept——复用 proxy 中转连接信息
5. ❌ 不使用 UD/DC 传输——仅用 RC
6. ❌ 不修改 `message.h` 中已有的 `GdsPutReq/GdsGetReq` 结构体

---

## 架构概览

```
Client (Us3TurboAccess)                         Backend (ufile-ac)
┌──────────────────────────┐         ┌──────────────────────────────────┐
│ RdmaTransferPath         │         │ RdmaService (新增)               │
│  (新增，替换 UcxPull)    │         │  ┌────────────┐  ┌────────────┐  │
│                          │         │  │ RdmaCtx     │  │ RdmaCtx    │  │
│ 1. ibv_reg_mr(buf, len)  │         │  │ (QP+CQ+PD)  │  │ (QP+CQ+PD) │  │
│ 2. export rkey + addr    │         │  │ per worker  │  │ per worker │  │
│ 3. send to proxy ────────┼────┐    │  └─────┬──────┘  └─────┬──────┘  │
│ 4. ack completion        │    │    │        │               │         │
└──────────────────────────┘    │    │  ┌─────┴───────────────┴──────┐  │
                                └───►│  │ PinnedBufferPool (复用)    │  │
                                     │  └───────────────────────────┘  │
Proxy (不改)                         │                                  │
  传输 metadata                         │  HandlePut:                      │
  (rkey, addr, qp_num, gid...)          │    buf = pool->Acquire(len)       │
                                        │    ibv_post_send(RDMA_READ,      │
                                        │       local=buf, remote=addr,     │
                                        │       rkey=remote_rkey, len=4MB)  │
                                        │    poll_cq() → 完成               │
                                        │    crc32c → disk write            │
                                        └──────────────────────────────────┘
```

### 关键设计决策

1. **复用 proxy 中转连接信息**：跟 UCX 一样，metadata 走 proxy（brpc），只有数据面走 RDMA
2. **复用 GDS 的 PinnedBufferPool**：已实现 pinned memory 分配 + `ibv_reg_mr`，可以直接复用
3. **RC 而非 DC**：RC 是标准 RDMA 传输，不需要 cuObjServer 的 DC 连接管理，API 更简单
4. **一个 worker thread 一个 QP**：避免锁竞争，跟现有线程模型一致
5. **连接建立通过 TCP out-of-band**：exchange QP num + LID/GID + QKEY，不走 RDMA CM

---

## 实现任务清单

### Phase 1: 后端基础设施（核心）

#### 任务 1.1: `ufile-ac/rdma/rdma_cq.h` — Completion Queue 封装

```cpp
// 目标：~80 行
// 职责：管理一个 ibv_cq，提供 poll_one 接口
// 接口：
//   - RdmaCq(ibv_context*, int max_cqe = 16)
//   - bool poll(timeout_ms) → 返回是否 poll 到 completion
//   - ibv_wc* last_wc() → 上次 poll 到的 work completion
//   - ibv_cq* native() → 裸指针
// 注意：CQ 绑定到每个 QP（每个 RdmaCtx 一个 CQ）
```

#### 任务 1.2: `ufile-ac/rdma/rdma_ctx.h` — QP + MR 上下文

```cpp
// 目标：~200 行
// 职责：管理一个 ibv_qp + 关联的 CQ + PD + 本地 MR，提供单次 RDMA READ/WRITE
// 接口：
class RdmaCtx {
 public:
  // 创建 QP：从 ibv_context, port_num, gid_index 初始化
  // 状态转换：RESET → INIT → RTR → RTS
  static Result<RdmaCtx> Create(ibv_context* ctx, uint8_t port, int gid_index);

  // 注册 host memory region（对标 ucp_mem_map）
  // 返回 (lkey, rkey, addr) 供 remote 使用
  ibv_mr* register_mr(void* buf, size_t len, int access = LOCAL_WRITE | REMOTE_READ | REMOTE_WRITE);

  // 单次 RDMA READ：从 remote QP 的 [remote_addr, remote_addr+len) 读到 local_buf
  // 内部：构造 ibv_send_wr + ibv_post_send
  // 返回：true 表示 post 成功（需要后续 poll CQ 确认完成）
  bool post_read(void* local_buf, size_t len,
                 uint64_t remote_addr, uint32_t remote_rkey);

  // 单次 RDMA WRITE：将 local_buf 写到 remote QP 的 [remote_addr, remote_addr+len)
  bool post_write(const void* local_buf, size_t len,
                  uint64_t remote_addr, uint32_t remote_rkey);

  // 轮询 CQ 直到特定 WR 完成或超时
  // 返回 WC status（IBV_WC_SUCCESS 表示成功）
  ibv_wc_status poll_one(int timeout_ms);

  // 连接信息：供 out-of-band 交换
  uint32_t qp_num() const;
  uint16_t lid() const;
  union ibv_gid gid() const;

  // 连接到 remote QP（用对方提供的 qp_num, lid, gid）
  // 将 QP 从 RTS 状态迁移到可以通信
  bool connect(uint32_t remote_qpn, uint16_t remote_lid, union ibv_gid remote_gid);

  // 析构：销毁 QP, CQ, PD
  ~RdmaCtx();
};
```

#### 任务 1.3: `ufile-ac/rdma/rdma_service.h` — 服务层

```cpp
// 目标：~300 行（.h + .cc）
// 职责：对标 GdsService 的 CPU 内存版本，管理 worker 线程池 + RdmaCtx 池
// 接口：完全对标 gds_service.h 的公开接口

struct RdmaPutContext {
  RdmaPutReq req;           // proxy 传来的请求（新增 message 类型）
  Message msg;
  std::string key;
  std::string rdma_token;   // client 侧 export 的 RDMA 连接信息
  
  uevent::ConnectionUeventPtr conn;
  PinnedBufferLease lease;  // 复用现有 PinnedBufferPool

  char* data;               // lease 的 data 指针缓存
  uint32_t crc32c;
  uint64_t bytes_written;
  int retcode;
  std::string errmsg;

  std::mutex mu;
  std::condition_variable cv;
  bool done;
};

class RdmaService {
 public:
  RdmaService(UfileAc* owner, uevent::EventLoop* loop);
  ~RdmaService();

  bool Start(const std::string& bind_host, int rdma_port, int worker_threads);
  void Stop();
  bool available() const;
  void SubmitPut(const std::shared_ptr<RdmaPutContext>& ctx);

 private:
  void WorkerLoop();
  void HandlePut(RdmaPutContext& ctx, RdmaCtx& qp);
  void SendResponse(const std::shared_ptr<RdmaPutContext>& ctx);

  UfileAc* owner_;
  uevent::EventLoop* loop_;
  std::vector<std::thread> workers_;
  // 每个 worker 独立的 RdmaCtx (QP+CQ)
  std::vector<std::unique_ptr<RdmaCtx>> qp_pool_;
  std::shared_ptr<PinnedBufferPool> pool_;  // 复用现有
  
  std::queue<std::shared_ptr<RdmaPutContext>> put_queue_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stopping_;
  std::string bind_host_;
  int rdma_port_;
};
```

#### 任务 1.4: `HandlePut` 核心流程

```cpp
// 伪代码（实际用 C++17）
void RdmaService::HandlePut(RdmaPutContext& ctx, RdmaCtx& qp) {
  // 1. 分配 pinned buffer（复用 GdsBufferPool）
  ctx.lease = pool_->Acquire(ctx.req.dataLen_);
  if (!ctx.lease.ok()) { /* error */ return; }
  ctx.data = static_cast<char*>(ctx.lease.data());

  // 2. 解析 client RDMA token
  //    token 格式: [qpn(4B)][lid(2B)][gid(16B)][rkey(4B)][addr(8B)]
  auto [remote_qpn, remote_lid, remote_gid, remote_rkey, remote_addr] =
      ParseRdmaToken(ctx.rdma_token);

  // 3. 连接 remote QP（modify QP to RTS with remote params）
  if (!qp.connect(remote_qpn, remote_lid, remote_gid)) { /* error */ return; }

  // 4. 单次 RDMA READ —— 关键：整个 4MB 一条 WQE
  if (!qp.post_read(ctx.data, ctx.req.dataLen_, remote_addr, remote_rkey)) {
    /* error */ return;
  }

  // 5. 轮询 CQ 等待完成
  ibv_wc_status status = qp.poll_one(io_timeout_ms_);
  if (status != IBV_WC_SUCCESS) { /* error */ return; }

  // 6. CRC32C
  ctx.crc32c = DoCrc32c(ctx.data, ctx.req.dataLen_, g_crc32c_type);

  // 7. 提交写盘
  loop_->QueueInLoop([this, ctx]() { owner_->PutFromRdma(ctx); }, true);
}
```

### Phase 2: 客户端（Client）

#### 任务 2.1: `Us3TurboAccess/client/src/transports/rdma/rdma_client.h` — RDMA Client 端

```cpp
// 目标：~200 行
// 职责：注册本地 host buffer → 导出 rkey + addr + QP info → 生成 rdma_token
//       对标 GDS 的 CuObjectClient，但仅用 libibverbs

class RdmaClientContext {
 public:
  // 初始化：打开设备 (mlx5_0)，创建 PD
  static Result<RdmaClientContext> Create(const std::string& device_name);

  // 注册 buffer（对标 ucp_mem_map + ucp_rkey_pack）
  struct RdmaBufferInfo {
    uint32_t rkey;
    uint64_t addr;
    uint32_t qp_num;
    uint16_t lid;
    uint8_t gid[16];
  };
  Result<RdmaBufferInfo> Register(const void* buf, size_t len);

  // 反注册
  void Deregister(const RdmaBufferInfo& info);

  // 导出为 transfer token（发给 proxy 的字符串）
  static std::string ExportToken(const RdmaBufferInfo& info);

 private:
  ibv_context* ctx_;
  ibv_pd* pd_;
  uint32_t qp_num_;    // 本地 QP number（用于对端连接）
  uint16_t lid_;
  union ibv_gid gid_;
};
```

#### 任务 2.2: `Us3TurboAccess/client/src/core/rdma/rdma_transfer_path.cpp` — TransferPath 实现

```cpp
// 目标：~250 行
// 职责：对标 UcxPullTransferPath，但用 RdmaClientContext 替代 UCX listener
//       实现 TransferPath 接口：PutObject / GetObject / available()
// 
// 关键流程（PutObject）：
//   1. RdmaClientContext::Register(buffer.data, buffer.size) → RdmaBufferInfo
//   2. ExportToken(info) → rdma_token 字符串
//   3. 构造 SessionHandshake（flow=CPUDirect, rdma_token）
//   4. proxy → ufile-ac（rdma_token 被透传）
//   5. ufile-ac 解析 token → ibv_post_send(RDMA_READ) → 完成
//   6. RdmaClientContext::Deregister(info)
//
// 注意：不需要 listener / accept / 反连！
//       连接信息全部通过 rdma_token 由 proxy 中转
```

### Phase 3: 协议与集成

#### 任务 3.1: `message.h` — 新增 `RdmaPutReq` / `RdmaPutRsp`

```cpp
// 对标 UcxPutReq 结构，用 rdma_token 替代 client_ucx_addr + packed_rkey
struct RdmaPutReq {
  uint32_t keyLen_;
  uint32_t tokenLen_;       // rdma_token 长度
  uint64_t dataLen_;
  uint64_t sourceOffset_;   // 在 remote buffer 中的偏移
  uint64_t requestId_;
  uint64_t sessionIdLow_;
  uint64_t sessionIdHigh_;
  uint32_t flags_;
  char data_[0];             // key bytes + rdma_token bytes
} __attribute__((packed));

struct RdmaPutRsp {
  int32_t retcode_;
  uint32_t crc32c_;
  uint64_t bytesWritten_;
  uint32_t errMsgLen_;
  char data_[0];             // errmsg bytes
} __attribute__((packed));
```

#### 任务 3.2: `ac_server.cc` — 集成 `RdmaService`

```cpp
// 在 ac_server.cc 中：
// - 新增 rdma_service_ 成员（对标 gds_service_）
// - Start() 中初始化 RdmaService
// - 新增 HandleRdmaPut() 解析 RdmaPutReq 并调用 rdma_service_->SubmitPut()
// - 新增 PutFromRdma() 回调（对标 PutFromGds）

// 这部分改动最小，因为接口完全对标 GdsService
```

#### 任务 3.3: `transfer_router.cpp` — 客户端路由

```cpp
// 在 TransferRouter 中新增 RdmaTransferPath
// CPUDirect flow:
//   if (rdma_path.available()) → rdma_path  (新)
//   else if (ucx_path.available()) → ucx_path  (旧，保留回退)
```

---

## 测试策略

### 单元测试（独立于 proxy/backend）

1. **`rdma_ctx_test`**：创建 QP → RESET → INIT → RTR → RTS → connect loopback → post_read → poll → 验证数据
2. **`rdma_client_register_test`**：Register buffer → 验证 rkey/addr 合法 → Deregister

### 集成测试（需要 proxy + backend）

3. **`rdma_put_4mb_test`**：客户端 4MB PUT → 后端 RDMA READ → 验证 CRC32C 匹配、数据内容一致
4. **`rdma_put_concurrency_test`**：8 并发 4MB PUT → 验证 QP 池分配正确、无锁竞争、吞吐 ~3 GiB/s
5. **`rdma_put_various_sizes`**：1KB / 64KB / 1MB / 4MB / 16MB → 验证不同大小正确性

### 回退测试

6. **`ucx_fallback_test`**：RDMA 路径不可用时自动回退 UCX（`available() == false`）

### 性能验证

7. **单次 4MB 延迟**：应 < 1ms（vs UCX 的 ~80ms）
8. **并发吞吐**：8 concurrent × 4MB 应 ~1 GiB/s（vs UCX 单流 35 MiB/s）

---

## rdma_token 格式

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          qp_num (32)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|             lid (16)          |           reserved (16)       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                                                               +
|                                                               |
+                          gid (128)                            +
|                                                               |
+                                                               +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          rkey (32)                            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                        remote_addr (64)                       +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
Total: 34 bytes, 可通过 base64 编码传给 proxy
```

---

## 文件清单

### 新建文件

| 文件 | 行数估算 | 说明 |
|------|---------|------|
| `ufile-ac/rdma/rdma_cq.h` | ~80 | CQ 封装 |
| `ufile-ac/rdma/rdma_ctx.h` | ~200 | QP + MR 上下文 |
| `ufile-ac/rdma/rdma_service.h` | ~120 | 服务层头文件 |
| `ufile-ac/rdma/rdma_service.cc` | ~300 | 服务层实现 |
| `Us3TurboAccess/client/src/transports/rdma/rdma_client.h` | ~200 | 客户端 RDMA 上下文 |
| `Us3TurboAccess/client/src/core/rdma/rdma_transfer_path.h` | ~60 | TransferPath 头文件 |
| `Us3TurboAccess/client/src/core/rdma/rdma_transfer_path.cpp` | ~250 | TransferPath 实现 |

### 修改文件

| 文件 | 改动量 | 说明 |
|------|--------|------|
| `ufile-ac/message.h` | +40 行 | 新增 RdmaPutReq/RdmaPutRsp |
| `ufile-ac/ac_server.cc` | +80 行 | 集成 RdmaService |
| `ufile-ac/ac_server.h` | +15 行 | 新增成员和回调声明 |
| `ufile-ac/CMakeLists.txt` | +5 行 | 链接 libibverbs，新增源文件 |
| `Us3TurboAccess/client/src/core/routing/transfer_router.cpp` | +10 行 | 路由到 RdmaTransferPath |
| `Us3TurboAccess/client/src/core/client/client_core.h` | +5 行 | 新增 RdmaClientContext 成员 |
| `Us3TurboAccess/client/src/core/client/client_core.cpp` | +20 行 | 初始化 RdmaClientContext |

### 总计
- **新建**：~1,210 行（比 UCX 的 990 行略多，但无外部依赖维护成本）
- **修改**：~175 行（最小化侵入）

---

## 实施顺序

1. **Phase 1（1.1 → 1.2）**：`rdma_cq.h` + `rdma_ctx.h` —— 可独立编译和单元测试，不依赖后端框架
2. **Phase 1（1.3 → 1.4）**：`rdma_service.h/cc` —— 集成到 ufile-ac，此时可用 mock client 测试
3. **Phase 2**：客户端 RDMA 组件 —— 可独立编译和单元测试
4. **Phase 3**：协议 + 路由集成 —— 端到端可用
5. **测试 + 性能验证**：先单流验证延迟 < 1ms，再并发验证吞吐
6. **替换 UCX**：验证通过后，CPUDirect flow 默认走 RDMA，UCX 保留为回退
