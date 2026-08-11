# Mooncake 接入 Us3Turbo 分析

> 日期：2026-08-11。范围：分析如何将 Mooncake（kvcache-ai/Mooncake）的底层存储/传输能力
> 接入 Us3Turbo（GDS+RDMA 双通路对象存储新架构）。Mooncake 仓已 clone 到
> `/mnt/us3_test/xinghui.shao/gds/Mooncake`（depth=1）。

---

## 1. 两者定位

### Us3Turbo（本仓）
GDS+RDMA 双通路对象存储。三层拓扑（README）：

```
client SDK / bench  ──brpc──►  us3_turbo_proxy (:9100)  ──TCP/RDMA──►  ufile-ac (:24000 TCP / :18666 GDS cuobj)
```

- **控制面**：brpc `Control` service（`proto/control_plane.proto`），client→proxy 全是元数据 RPC
  （`GdsPut`/`RdmaPut`/`CreateMultipartUpload`/`UploadPart{Gds,Rdma}`/`Complete`/`StatObject`/`GdsGet`/`RdmaGet`）。
- **数据面**：ufile-ac 后端（独立仓 `ggds-compile-env/ufile-ac`）是**主动搬运方**——
  解码 client 传来的 `rdma_token`，反向连接 client，`ibv_post_send(RDMA_READ)`（PUT）
  或 `RDMA_WRITE`（GET），再落 NVMe + 写索引。
- **持久存储**：ufile-ac 的 NVMe（`AllocWriteOffset`/`WriteData`）+ 索引（keysmap + dbgate mongo fileidx）。

### Mooncake
KVCache-centric 分离式架构（FAST'25 Best Paper）。与"底层存储能力"直接相关的两个子系统：

- **Transfer Engine（TE）**：传输无关的数据搬运原语。10+ transport 后端
  （`rdma`/`tcp`/`efa`/`nvmeof`（cuFile/GPUDirect Storage）/`device`（GPU P2P + IBGDA）
  /`nvlink`/`hip`/`ascend`/`cxl`/`ub`/`cxi`…）。自包含 RDMA 引擎：自己 `ibv_reg_mr`、
  建 QP、post send/recv、poll CQ、多 NIC 池化、拓扑选路、故障转移。需要 metadata service
  （`etcd://` / `redis://` / 内置 HTTP / `P2PHANDSHAKE`）做 peer/segment 发现。
- **Mooncake Store**：建在 TE 之上的分布式 KVCache 存储层。Master（gRPC）协调 replica 放置、
  lease、驱逐；Client 双角色（API 调用方 + 内存 segment 提供方）。多级存储
  （DRAM/VRAM segment → 本地 SSD offload，后端 SPDK/cachelib/hf3fs）。
  ⚠ Store 的存储层是**本地 DRAM/SSD**，不是一个可插拔的远端对象存储后端。

公共 API（已核对头文件）：
- TE：`mooncake-transfer-engine/include/transfer_engine.h`、`transport/transport.h`
  - `TransferEngine::init(metadata_conn_string, local_server_name, ip, rpc_port)`
  - `installTransport(proto, args)` → `Transport*`
  - `registerLocalMemory(addr, length, location, remote_accessible, update_metadata)`
  - `openSegment(name)` / `closeSegment(handle)`
  - `allocateBatchID(n)` / `submitTransfer(batch_id, vector<TransferRequest>)` /
    `getTransferStatus(batch_id, task_id, status)` / `submitScatter(...)`
  - `struct TransferRequest { OpCode{READ,WRITE}; void* source; SegmentID target_id; uint64_t target_offset; size_t length; }`（`transport/transport.h:60`）
- Store：`mooncake-store/include/client_service.h`
  - `Client::Create(local_hostname, metadata_connstring, protocol, device_names, master_entry, transfer_engine)`
  - `Put(ObjectKey, vector<Slice>&, ReplicateConfig)` / `Get(object_key, slices)` / `Query`
  - `MountSegment(buffer, size, protocol, location)` / `RegisterLocalMemory(...)`

---

## 2. Us3Turbo 当前数据面剖析（接入点定位）

关键事实（来自 `client/src/transport/*`、`client/src/memory_manager/*`、
`proxy/src/storage/*`、`ufile-ac/rdma_service.h`）：

1. **client 是被动 RDMA target**：`RdmaMemoryManager::AcquireDescriptor`
   → `ibv_reg_mr(IBV_ACCESS_REMOTE_READ)`，开 listener（`RdmaQp::CreateListener`，
   后台 `AcceptLoop` 接受 backend 反连），把 `{ip,port,rkey,addr,size}` 打包成 token
   （`EncodeToken`，`rdma_qp.cpp:266`）。GDS 路径用 cuObj（`cuObjClient::cuMemObjGetRDMAToken`，
   `gds_memory_manager.h:64`），**不是 cuFile**。
2. **proxy 是纯控制面、零字节接触**：`UfileAcClient::PutBlockGds/PutBlockRdma`
   只把 `{key, rdma_token, offset, len}` 走 ufile-ac 私有二进制 TCP 协议
   （`ufile_ac_protocol.h`：`Message`(52B) + `GdsPutReq`/`RdmaPutReq`）发给 backend。
3. **backend 是唯一主动搬运方**（`ufile-ac/rdma_service.h` 注释）：
   `DecodeToken → RdmaQp::Connect（反连 client）→ PostRead(RDMA_READ) → CRC32C →
   PutFromRdma(AllocWriteOffset + WriteData + 写索引) → SendResponse`。GET 反向 `RDMA_WRITE`。
4. **两套手写 verbs 实现互为镜像**：client 侧 `client/src/memory_manager/rdma_qp.{h,cpp}` 与
   backend 侧 `ufile-ac/rdma/rdma_qp.{h,cc}` 各自实现 token 编解码 + listener + QP 池，
   单 NIC、无故障转移、需手工对齐编码格式。GDS 路径同理依赖 cuObj 双端实现。

> 即：Us3Turbo 的"底层存储/传输能力"= ① 搬运原语（手写 verbs ×2 + cuObj ×2）+
> ② 持久层（ufile-ac NVMe + 索引）。Mooncake 能替换的是 ①，② 是 Mooncake 没有的
> （Store 是 DRAM/SSD 缓存语义，非对象持久化）。

---

## 3. 三种接入方向

> 用户原话"将 kvcache 的底层存储能力**接入 Us3Turbo**"——方向是 Us3Turbo 作为消费方。
> 下表 A/B 为正方向；C 为反方向（Us3Turbo 作为 Mooncake 的叶子），列出供对比。

| 方向 | Mooncake 角色 | 改动面 | 适配度 | 风险 |
|---|---|---|---|---|
| **A. TE 替换手写 RDMA 搬运** | 数据搬运原语 | client mem-mgr + backend rdma_service | ★★★★ | 中 |
| **B. Store 作为缓存层** | DRAM/VRAM/SSD KVCache tier | proxy 新增 cache 层 | ★★ | 高（语义错配） |
| **C. Us3Turbo 暴露为 TE transport** | 远端 segment 后端 | Mooncake 侧加 transport 插件 | ★（反方向） | 中 |

### 方向 A：Transfer Engine 替换手写 RDMA 搬运（推荐）

**为什么贴合**：Us3Turbo 的 "backend 主动搬、client 被动靶" 模型与 TE 的
`submitTransfer` 模型同构——TE 调用方就是主动搬运方，对端只把本地内存注册成 segment（被动靶）。
Us3Turbo 当前的 `rdma_token`（`ip:port+rkey+addr+size` 的打包串）本质就是一个**手写 segment descriptor**，
Mooncake 把它形式化成 `(segment_name, offset, size)`，经 metadata service 解析成完整 `BufferDesc{addr, rkey, protocol}`。
token 在 proxy 里的"随 RPC 透传"角色不变，只是编码从私有二进制换成 segment handle。

**角色映射**：
- client 侧跑一个 TE 实例：`registerLocalMemory(gpu_ptr/host_ptr, size, "cuda:0"/"cpu:0", remote_accessible=true)`
  → 开一个 segment（per-upload 或 per-part），把 segment_name 透过 proxy 传给 backend。
- backend 侧跑一个 TE 实例：`openSegment(client_segment_name)` →
  `allocateBatchID` → `submitTransfer({opcode=READ, source=本地落盘buffer, target_id=client_segment, target_offset, length})`
  （PUT 拉取）/ `WRITE`（GET 推送）→ `getTransferStatus` → 落 NVMe + 索引。
- metadata service：`P2PHANDSHAKE`（无外部服务，握手期交换 descriptor 并本地缓存）即可起步，
  后续按需升级 etcd/redis。Us3Turbo 已有 mongo（dbgate），亦可把 segment descriptor 存 mongo 复用。

**收益**：
- 消除 client/backend 两套手写 verbs 互为镜像的维护负担，统一到一个成熟引擎。
- 免费获得多 NIC 池化、拓扑选路、RDMA 故障转移——直击 Us3Turbo 的 GDS+RDMA 双通路诉求。
- GPU 内存支持现成：TE 的 `device_transport`（IBGDA / GPU 主动 RDMA）+ DMA-BUF/nvidia-peermem 注册，
  与 Us3Turbo "device 显存" 通路语义一致。

**摩擦点（务必先验证）**：
1. **GDS 路径的 cuObj ≠ Mooncake 的 cuFile/NVMe-oF**。Us3Turbo GDS 走 cuObj（`cuobjserver`，
   NVIDIA 专有，token 由 cuObj 签发），Mooncake GDS 走 `nvmeof_transport`（cuFile/NVMe-oF）+
   `device_transport`（IBGDA）。两者是不同原语，不能直接互换 token。
   → 建议**分通路接入**：先 RDMA 通路（libibverbs ↔ TE `rdma_transport`，模型完全对齐），
   GDS 通路保留 cuObj 或单独评估迁移到 TE `device/nvmeof`（属二期、需重测 perf）。
2. **token 线协议**：proxy→backend 现走 ufile-ac 私有二进制（`Message`+`GdsPutReq`）。
   若 backend 换成 TE，token 字段语义从"packed ip:port+rkey+addr+size"变成"segment_name+offset+size"，
   需同步改 `ufile_ac_protocol.h` 的 req 结构与 backend `DecodeToken`。若想**不动 client/proxy**，
   可在 backend 侧写一个 adapter：仍收旧 token，内部 translate 成 TE segment 调用（最小侵入，见下）。
3. **零接触控制面须保留**：不能把 TE 放到 proxy（会让 proxy 碰字节，破坏零拷贝 pull 设计，
   perf 回退）。TE 实例只放 client 端和 backend 端。

**最小侵入落地（backend-only adapter）**——首版推荐：
- client / proxy **完全不动**。
- 在 `ggds-compile-env/ufile-ac` 内新增 `MooncakeRdmaService`（与现有 `rdma_service` 并存，
  ini 选择），实现 `RdmaPut/RdmaGet`：解码旧 token 拿到 `{ip,port,rkey,addr,size}` →
  不再自己 `ibv_post_send`，而是用 TE `rdma_transport` 提交 READ/WRITE（把 client buffer
  当 remote segment 注册到 backend 的 TE，或用 TE 的 "dynamic remote segment" 能力直接喂 rkey+addr）。
- 收益直接验证：backend 单 NIC verbs → TE 多 NIC + 故障转移，不改 wire 协议、不改 client/proxy。
- 风险隔离：ufile-ac 是独立仓、本仓 review 不动（见 [[us3turbo-test-framework]] "只改不提交"），
  adapter 落在 ufile-ac 分支验证。

**全量落地（二期）**——client 侧也接 TE：
- `client/src/memory_manager/rdma_memory_manager` 由"手写 verbs + listener"换成"TE segment 注册"，
  `rdma_token` 字段换成 `segment_name + offset`。proxy `ufile_ac_protocol` 同步。
- 彻底删 `client/src/memory_manager/rdma_qp.{h,cpp}` 与 backend `rdma/rdma_qp.{h,cc}` 一侧的 verbs 代码。
- 此时 GDS 路径的 cuObj 决策点见摩擦点 1。

### 方向 B：Mooncake Store 作为 Us3Turbo 的缓存层（不推荐首版）

设想：proxy → Mooncake Store（DRAM/VRAM/SSD tier）→ ufile-ac（冷后备）。热对象走 Store，
miss 落 ufile-ac。

**为什么不贴合**：
- Store 的客户端模型是"**cooperative client**"——KV cache 工作负载里，serving engine（vLLM/SGLang）
  自己挂 segment、自己当 TE 调用方。Us3Turbo 的 client 是**被动对象存储客户端**（不挂 segment、
  不主动搬），角色错配。
- Store 的对象模型是 `ObjectKey + vector<Slice>` + replica 放置/驱逐/lease，是**临时 KV cache 语义**；
  Us3Turbo 是 bucket/key + multipart + 持久对象 + ETag/CRC，语义不对齐。把 Store 当持久层
  会丢失 Us3Turbo 的对象存储语义，当缓存层则要自写一套 miss→ufile-ac 的回填逻辑。
- Store 没有现成的"远端对象存储后端"插件（它的 L2 是本地 SPDK/cachelib/hf3fs）。

**若一定要做**：把 Store 部署为 proxy 侧的 L1/L2 读缓存（纯 DRAM/SSD，命中即返），
miss 时 proxy 回退 `UfileAcClient`。但要解决：Store 对象生命周期与 ufile-ac 索引一致性、
写穿透/写回策略、multipart 与 Store slice 切分对齐。工作量大、收益不明确，**留作三期评估**。

### 方向 C：Us3Turbo 反向暴露为 Mooncake transport（仅列出）

在 `mooncake-transfer-engine/src/transport/` 新增 `us3turbo_transport`，把 Us3Turbo 的
bucket/key 对象当成一个远端 "segment"，让 vLLM/SGLang 等 Mooncake 生态用 Us3Turbo 做存储后端。
这是"把 Us3Turbo 接入 Mooncake"而非"把 Mooncake 接入 Us3Turbo"，与用户原话方向相反，
但若产品目标是"让 Us3Turbo 服务 KV cache 工作负载"则值得单独立项。延迟特性上 Us3Turbo
（对象存储、NVMe 落盘）与 Mooncake 目标的亚毫秒 RDMA KV cache 不在同一档，需先量化。

---

## 4. 推荐方案与分阶段

| 阶段 | 目标 | 改动位置 | 验证 |
|---|---|---|---|
| **P0 验证** | TE `rdma_transport` 能跑通 Us3Turbo 的 pull/push | ufile-ac 内 adapter（`MooncakeRdmaService`），收旧 token 转 TE 调用 | 复用 `rtest/bench/rdma/us3_turbo_bench_rdma_multipart`，对比手写 verbs 吞吐 |
| **P1 RDMA 全量** | client 侧 mem-mgr 换 TE，删 client/backend 两侧 `rdma_qp` verbs | `client/src/memory_manager/rdma_*`、`ufile_ac_protocol.h` token 字段 | RDMA 通路 rtest 全绿（8/12 基线见 [[us3turbo-test-framework]]） |
| **P2 GDS 决策** | 评估 cuObj → TE `device/nvmeof` 迁移 vs 保留 cuObj | `gds_memory_manager`、backend `gds_service` | GDS bench 对比（最优 part=8M/nt=16/cp=16） |
| **P3（可选）** | Store 作为 proxy 侧读缓存 | proxy 新增 cache 层 | miss 率/命中加速比 |

P0 是最小风险验证：**只动 ufile-ac 一个独立仓的 adapter、client/proxy 不动**，
能最快回答"TE 的多 NIC/故障转移对 Us3Turbo RDMA 通路是否有增益"。

---

## 5. 待决策

1. **接入方向**：A（TE 替换搬运，推荐）/ B（Store 缓存层）/ C（反向暴露）——
   或三者关系（A 先行，B/C 后续）。
2. **GDS 路径**：首版是否保留 cuObj，只接 RDMA 通路？（强烈建议是）
3. **metadata service**：P0/P1 用 `P2PHANDSHAKE`（零依赖）还是直接上 etcd/复用 mongo？
4. **改动落点**：是否允许动 `ggds-compile-env/ufile-ac`（独立仓，"只改不提交"原则），
   还是只在 Us3Turbo 本仓做 client/proxy 侧适配，backend 侧等独立评审？
