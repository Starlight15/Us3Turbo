# RDMA vs GDS 性能瓶颈分析

**日期**: 2026-07-30
**数据来源**: `docs/基准测试_4MB_多轮稳定.md`（4MB 单步 PUT，proxy `--num_threads=8`）

---

## 1. ops/s 与带宽 (MiB/s) 对比

> 带宽 = ops/s × 4 MiB（每 PUT 一个 4MB 对象）

### 1.1 无磁盘写入 (mock_aio_write=1)

| conc | GDS ops/s | GDS MiB/s | RDMA ops/s | RDMA MiB/s | GDS/RDMA 倍数 |
|:----:|:---------:|:---------:|:----------:|:----------:|:-------------:|
| 1    | ~115      | 460       | **108**    | 432        | 1.1×         |
| 4    | ~285      | 1140      | **201**    | 804        | 1.4×         |
| 8    | **606**   | 2424      | ~356       | 1424       | 1.7×         |
| 16   | ~998      | 3992      | ~608       | 2432       | 1.6×         |
| 32   | ~1455     | 5820      | ~1034      | 4136       | 1.4×         |

### 1.2 真实磁盘写入 (mock_aio_write=0)

| conc | GDS ops/s | GDS MiB/s | RDMA ops/s | RDMA MiB/s | GDS/RDMA 倍数 |
|:----:|:---------:|:---------:|:----------:|:----------:|:-------------:|
| 1    | **93**    | 372       | **95**     | 380        | 1.0×         |
| 4    | **387**   | 1548      | ~270       | 1080       | 1.4×         |
| 8    | **744**   | 2976      | **310**    | 1240       | 2.4×         |
| 16   | ~876      | 3504      | **518**    | 2072       | 1.7×         |
| 32   | ~1316     | 5264      | ~885       | 3540       | 1.5×         |

> **关键观察**: RDMA 在 conc=8 w/disk 时差距最大 (GDS 2.4×)，无磁盘时差距在 1.4-1.7× 范围。低并发 (conc=1) 时基本持平。

---

## 2. 瓶颈分析

### 2.1 根因总览

GDS 和 RDMA 两条 PUT 路径在 ufile-ac 侧的关键差异：

| 阶段 | GDS (cuObj) | RDMA (libibverbs) | RDMA 开销 |
|------|------------|-------------------|:---------:|
| 缓冲区 | **预分配池** (`PinnedBufferPool`) | **每次分配** (`posix_memalign`) | 🔴 高 |
| MR 注册 | **预注册** (池初始化时通过 cuObjServer) | **每次注册+注销** (`ibv_reg_mr` / `ibv_dereg_mr`) | 🔴🔴🔴 极高 |
| 数据传输 | `server_->handlePutObject` (cuObj RDMA READ) | `PostRead` + `PollOne` (ibv_post_send + ibv_poll_cq) | 🟡 相近 |
| QP 管理 | cuObjServer 内置管理 | AcquireQp/ReleaseQp + pool_mu_ 锁 | 🟡 中等 |
| CRC32C | 相同 | 相同 | 🟢 无差异 |

### 2.2 最大瓶颈: 每次请求的 MR 注册/注销

**代码位置**: `rdma_service.cc:255-264, 303`

```cpp
// 每条 RDMA PUT 请求都要做:
ibv_mr* local_mr = qp->RegisterMr(ctx->data, data_len, ...);  // ← kernel call: pin pages + HCA register
// ... RDMA READ ...
qp->DeregisterMr(local_mr);  // ← kernel call: unpin pages + HCA unregister
```

`ibv_reg_mr` 是一个系统调用，需要：
1. 锁定物理内存页（pin）
2. 向 HCA (ConnectX-6) 注册内存区域
3. 返回 lkey/rkey

对于 4MB 缓冲区，这个操作通常耗时 **10-100 μs**，取决于内存碎片程度。加上对应的 `ibv_dereg_mr`，每条请求仅 MR 管理就额外消耗 **20-200 μs**。

**对比 GDS**: `PinnedBufferPool` 在初始化时通过 `cuObjServer` 预先注册所有缓冲区 (`gds_buffer_pool.h`)，每条请求的 `Acquire()` 只是从池中取出一个预注册的 buffer，不需要任何 kernel call。且 `Release()` 归还 buffer 时**保留 MR 注册状态**（`gds_buffer_pool.cc:141-177`），不会 deregister。

### 2.3 次大瓶颈: 每次请求的 buffer 分配

**代码位置**: `rdma_service.cc:213-215`

```cpp
int pm_ret = posix_memalign(reinterpret_cast<void**>(&ctx->data),
                            MEM_ALIGNMENT_SIZE, data_len);
// ... 使用完后在 ~RdmaPutContext() 中 free
```

`posix_memalign` 4MB 对齐分配通常需要 `mmap` 或从堆中查找，耗时 **5-50 μs**。更重要的是，新分配的 page 在 RDMA READ 写入时会触发 **first-touch page fault**。

**对比 GDS**: `PinnedBufferLease` 从预分配池中 O(1) 获取，只需加锁取队首元素，耗时 < 1 μs，且页面已预先 fault-in。

### 2.4 中等瓶颈: QP 连接池锁竞争 + 容量不足

**代码位置**: `rdma_service.cc:159-170, 177-190`

```cpp
// AcquireQp 和 ReleaseQp 都持有全局 pool_mu_
std::lock_guard<std::mutex> lk(pool_mu_);
```

**关键问题**: `kMaxQpsPerClient = 4`（`rdma_service.cc:154`），但 worker 有 **8 个**。当 4 个 QP 都在使用时：
- 第 5-8 个请求调用 `AcquireQp` → pool MISS → 执行 `RdmaQp::Connect()` 建立新的 RDMA CM 连接
- 完成后 `ReleaseQp` 发现池已满 → 直接 **`delete` QP**（`rdma_service.cc:187-188`）
- 下一个请求再次重复 `Connect → use → delete` 循环

**RDMA CM Connect 开销**：
- TCP 三次握手 + RDMA CM 消息交换（`rdma_resolve_addr` → `rdma_resolve_route` → `rdma_create_qp` → `rdma_connect`）
- `WaitEvent` 使用 `select()` 以 **100ms 粒度轮询**（`rdma_qp.cc:131-132`）
- 每次新连接耗时 **1-5 ms**，是 per-request MR 注册的 10-50 倍

加上 `put_queue_` 的 `mu_` 锁（`rdma_service.cc:87-98`），每条请求至少涉及 **3 次互斥锁操作**：
1. `SubmitPut` 入队 (`mu_`)
2. `WorkerLoop` 出队 (`mu_`)
3. `AcquireQp` + `ReleaseQp` (`pool_mu_`)

### 2.5 RC vs DC 传输模式差异

| 特性 | RDMA (RC) | GDS (DC) |
|------|-----------|----------|
| 传输类型 | `IBV_QPT_RC` (Reliable Connected) | `CUOBJ_PROTO_RDMA_DC_V1` (Dynamically Connected) |
| 连接模型 | 每 client 需独立 QP + CM 握手 | cuObjServer 单一持久连接 |
| 并发模型 | 多个 RC QP (最多缓存 4 个) | Lock-free channel（无需额外连接） |
| 连接建立 | `RdmaQp::Connect` + select() 轮询 | 初始化时完成，运行时无开销 |

DC (Dynamically Connected) 传输允许多个远端通过同一 QP 通信，不需要 per-peer 连接。RDMA 使用的 RC 传输则需要 per-peer QP + 完整的 CM 握手流程，在高并发下成为瓶颈。

### 2.6 轻微瓶颈: 跨线程调度 + double-buffer memcpy

**代码位置**: `rdma_service.cc:311, 322`；`ac_server.cc:1762-1772`

```cpp
loop_->QueueInLoop([this, ctx]() { owner_->PutFromRdma(ctx); }, true);  // 调度到主 event loop
// ... wait on cv ...
loop_->QueueInLoop([this, ctx]() { SendResponse(ctx); }, true);     // 再次调度
```

每条请求有 2 次跨线程 `QueueInLoop` 调度。此外，`PutFromRdma` 在主 event loop 线程上进行 **第二次 `posix_memalign` + `memcpy`**（`ac_server.cc:1762` — 将数据从 RDMA buffer 拷贝到 `dataHeader_`），这是 GDS 和 RDMA 共享的开销，但对 RDMA 而言意味着：data 刚从 client host memory RDMA READ 到 ufile-ac buffer，又立即被 memcpy 到另一个 buffer。

### 2.7 RDMA 自带的 PHASE 日志存在误导

**代码位置**: `rdma_service.cc:327-334`

```cpp
ULOG_INFO << "HandleRdmaPut DONE ..."
          << " pool_us=" << us(t_alloc, t_connect)     // ← 包含 QP Connect!
          << " rdma_read_us=" << us(t_connect, t_rdma) // ← 包含 RegisterMr + PostRead + PollOne
          << " crc32_us=" << us(t_rdma, t_crc)         // ← 包含 DeregisterMr + ReleaseQp!
```

- `pool_us` 实际 = `DecodeToken` + `AcquireQp`（含可能的 Connect），不只是"pool 操作"
- `rdma_read_us` 实际 = `RegisterMr` + `PostRead` + `PollOne`，**MR 注册开销隐藏在 RDMA READ 耗时中**
- `crc32_us` 实际 = `DeregisterMr` + `ReleaseQp` + `DoCrc32c`，**MR 注销开销隐藏在 CRC 耗时中**

这意味着仅凭现有日志无法精确量化 MR 注册/注销的独立开销，需要像 GDS 一样做更细粒度的分段埋点（参考 `US3_GDS_PUT_TIMING` 环境变量机制）。

### 2.8 为什么 conc=1 时差距最小？

| 阶段 | conc=1 时的影响 |
|------|----------------|
| MR 注册 | 每次都要做，但单线程无竞争 |
| Buffer 分配 | 每次都要做，但无并发分配压力 |
| 锁竞争 | 单 worker，无竞争 |
| QP 池 | 只需 1 个 QP，始终命中缓存 |

当并发度增加时，这些 per-request 开销叠加锁竞争，RDMA 的吞吐天花板远低于 GDS。

---

## 3. 数据旁证：从 ufile-ac 日志验证

RDMA PUT 的 PHASE 日志（`rdma_service.cc:327-334`）：

```
HandleRdmaPut DONE key:...
  PHASE alloc_us=N      ← posix_memalign
       pool_us=N        ← DecodeToken + AcquireQp (含可能的 Connect)
       rdma_read_us=N   ← RegisterMr + PostRead + PollOne  ← MR注册隐藏于此!
       crc32_us=N       ← DeregisterMr + ReleaseQp + DoCrc32c ← MR注销隐藏于此!
       disk_wait_us=N
       total_us=N
```

要精确量化 MR 注册开销，可在 `RegisterMr`/`PostRead`/`PollOne`/`DeregisterMr` 前后加埋点，像 GDS 的 `US3_GDS_PUT_TIMING` 那样分段计时。预估 MR 注册+注销合计占 `rdma_read_us + crc32_us` 的 20-40%。

---

## 4. 优化建议（按投入产出比排序）

### P0: RDMA buffer pool（消除 MR 注册开销）🔴🔴🔴

**方案**: 仿照 GDS 的 `PinnedBufferPool`，为 RDMA 实现 buffer pool，预注册 MR。

```
class RdmaBufferPool {
  struct Entry { void* data; ibv_mr* mr; };
  // 初始化时为每种 size class 预分配 + 预注册 N 个 buffer
  // Acquire() 返回预注册的 (data, mr)
  // Release() 归还池中（不 deregister）
};
```

**预期收益**: 消除 per-request `ibv_reg_mr`/`ibv_dereg_mr` + `posix_memalign`/`free`，预计 **RDMA 吞吐提升 40-80%**。

### P1: 增大 QP 池容量 🟡

**方案**: `kMaxQpsPerClient` 从 4 调至 8（匹配 worker 数），避免高并发下频繁创建新连接。

**预期收益**: 高并发 (conc≥16) 时减少 RDMA CM 连接建立开销，提升 10-20%。

### P2: 减少锁竞争 🟡

**方案**: 
- QP 池改用 per-client 锁或无锁结构
- 请求队列改用 lock-free SPSC 队列

**预期收益**: 高并发下降低锁等待，提升 5-15%。

### P3: QP 亲和性绑定 🟢

**方案**: 每个 worker 绑定专属 QP，避免跨 worker 的 QP 争抢。

**预期收益**: 减少 QP 状态切换，提升 5-10%。

---

## 5. 结论

1. **RDMA 性能差距的根本原因不是 RDMA 传输本身慢，而是 per-request 的内存管理开销** — 每次请求都要分配 buffer + 注册 MR + 注销 MR + 释放 buffer，而 GDS 通过预注册 buffer pool 完全消除了这些开销。

2. **MR 注册 (`ibv_reg_mr`) 是最大单一瓶颈** — 这是一个 kernel call，每次耗时 10-100 μs。4MB PUT 整体耗时约 1000-5000 μs（取决于是否写磁盘），MR 注册占比可达 5-20%。

3. **低并发时差距小 (1.0-1.1×)，高并发时差距大 (1.4-2.4×)** — 因为 per-request 开销固定，低并发时 RDMA 传输效率与 GDS 接近；高并发时固定开销叠加锁竞争使得 RDMA 吞吐提前到达天花板。

4. **实现 RDMA buffer pool (P0) 是最有效的优化**，预计可将 RDMA 吞吐提升至 GDS 的 80-90% 水平。
