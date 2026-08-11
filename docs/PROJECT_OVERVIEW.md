# Us3Turbo 项目介绍

> 面向高性能场景的 GDS + RDMA 双通路对象存储。

**测试日期**: 2026-08-11
**文档定位**: 项目级总览。逐层数据与复测方法见 `GDS_REAL_RW_REPORT.md`、`RDMA_PERF_REPORT.md`、`TUNING_DEEP_ANALYSIS.md`;冷层接入场景见 `MOONCAKE_OFFLOAD_SOLUTION.md`。

---

## 1 项目简介

### 1.1 系统定位

Us3Turbo 是一个把 **GPUDirect Storage (GDS)** 与 **RDMA (RoCE/InfiniBand)** 两条高速数据通路集成进对象存储语义的系统。它对外提供单步 PUT / 分段上传 / GET 三类对象存储接口,对内把数据搬运下沉到 GPU 显存直通(GDS cuObj)或主机内存 RDMA(libibverbs RC + RDMA CM),控制面统一走 brpc proxy。

**定位**:让 GPU / 主机内存与远端 NVMe 之间不再经过内核 TCP 协议栈和多次内存拷贝,把对象存储的易用接口接到 RDMA/GDS 的裸带宽上。

### 1.2 传统对象存储 vs 高性能对象存储

传统对象存储(以 S3 兼容的云对象存储、Ceph RGW、自建 HTTP 对象存储为代表)为"通用、弹性、多租户"而生,数据路径长:

| 维度 | 传统对象存储 | 高性能对象存储 (Us3Turbo) |
|---|---|---|
| **访问协议** | HTTP/HTTPS + REST(S3 API) | brpc 控制面 + RDMA/GDS 数据面 |
| **数据路径** | 应用 → 内核 socket → TCP/IP 栈 → 用户态 web server → 内核 socket → 落盘,多段 `read/write`/`sendfile` 拷贝 | 应用 GPU/主机内存 → RDMA READ/WRITE 直达 backend NVMe,**零拷贝、绕内核 TCP** |
| **数据源** | 普通主机内存 buffer | **GPU 显存(GDS)** 或主机内存(RDMA),前者连 GPU 显存拷贝都省 |
| **单 RTT 延迟** | 百 µs ~ ms 级(含 TCP 握手、TLS、HTTP 解析) | µs 级 RDMA READ(RDMA READ 4M ~0.9 ms 端到端) |
| **单流带宽** | 受 TCP/单连接与 web server 串行化约束,常 1–3 GB/s 封顶 | 直打 NVMe 写带宽天花板(本机 ~3.3–3.9 GB/s)或 host 搬运上限(mock ~10 GB/s) |
| **CPU 开销** | 每字节多 µs 的协议栈/加密/序列化开销,高并发即 CPU-bound | backend 真写仅 ~1 核(110% / 128 核),CPU 富余 |
| **语义模型** | 全量 PUT + multipart,part 较大(5–数 GB),看重持久化与一致性 | 同 S3 语义(单步 + multipart + GET),但 part 较小(4M/8M)以最大化并行度、打满 RDMA 队列 |
| **适合负载** | 通用文件、备份、静态资源、Web 资产 | 任何对单流带宽 / per-object 延迟 / CPU 旁路 / GPU 显存直通有诉求的高性能场景(AI checkpoint、HPC、大数据入湖、分布式冷层、高码率媒体等) |

核心差异不在"存了什么",而在**数据搬运的路径深度**:

- 传统链路里,一次对象写要穿过"应用 buffer → 内核 send buffer → 网卡 → 交换 → 网卡 → 内核 recv buffer → 用户态 server → 再 write 到 NVMe"。每一跳是一次内存拷贝或上下文切换,且 TCP/IP 协议栈本身吃 CPU。
- Us3Turbo 里,GPU 显存的数据由 backend 用 **RDMA READ** 直接拉走并落盘——GPU 上的训练张量不必先 `cudaMemcpy` 到主机内存再 `send`;主机内存的 buffer 也由 backend 用 RDMA READ 直读、用 RDMA WRITE 直写(GET)。中间没有 `read`/`write` 系统调用的数据拷贝,RDMA 旁路内核,TCP 协议栈不参与数据搬运(同机部署甚至走网卡内部 loopback,不经物理链路)。

**适用场景**:数据生产者/消费者已在 GPU 或主机内存中、单对象 GB 级、对单流带宽和 per-object 延迟敏感的高性能负载。定位为**通用高性能对象存储底座**,不限具体业务:

- **AI 训练 / 推理**:checkpoint 落盘、KV cache offload(见 `MOONCAKE_OFFLOAD_SOLUTION.md`)、模型权重分发、GPU 显存大数据集直取——数据天然在显存,GDS 通路连 `cudaMemcpy` 都省。
- **HPC / 科学计算**:大规模模拟输出、气象/地质/基因组数据的中间产物在节点间流动,RDMA 通路打满 RNIC 带宽,避免 TCP 协议栈吃 CPU。
- **大数据 / 流计算**:shuffle 中间态、列存快照的高速入湖与回读,单流带宽逼近 NVMe 天花板,降低流水线 stall。
- **分布式存储 / 缓存层**:作为持久、跨集群、可扩容的远端冷层或共享数据层,补足"快但节点本地"介质的持久性与跨节点共享能力。
- **视频 / 媒体处理**:高码率素材的高速 ingest 与分发,单流吞吐不再是瓶颈。

综上,对上述任一项有诉求的场景,均属于 Us3Turbo 的目标负载。

---

## 2 系统架构

### 2.1 三层拓扑

```
                控制面 (brpc, protobuf)                       数据面 (RDMA / GDS)
┌──────────────┐  ──────────────────────►  ┌──────────────────┐  ──────────────────────►  ┌─────────────────┐
│  Client SDK  │   Control service:        │  us3_turbo_proxy │   ufile_ac_protocol       │   ufile-ac       │
│  + bench     │   PutObject / Multipart   │   (brpc :9100)   │   (TCP :24000)            │   backend         │
│  (本仓)       │   / Get / Stat           │   (本仓)          │   GDS cuObj (:18666)      │  (独立仓)         │
└──────────────┘  ◄──────────────────────  └──────────────────┘  ◄──────────────────────  └─────────────────┘
   GDS: cuObj token                                   put/get block                    RDMA READ client 显存/内存 → NVMe
   RDMA: libibverbs                                   by block_key                       GET: RDMA WRITE 回 client
   ↑ 反向 listener 供 backend RDMA CM 连回
```

| 层 | 进程 | 端口 | 仓 | 职责 |
|---|---|---|---|---|
| **控制面 + 转发** | `us3_turbo_proxy` | 9100 (brpc) | 本仓 | 接收 client RPC、校验、生成 `upload_id`/`trace_id`、转发 backend、写 part 索引、组装 GET |
| **数据面 backend** | `ufile-ac` | 24000 (TCP 协议)、18666 (GDS cuObj) | `ggds-compile-env`(独立仓) | RDMA READ 搬运 + NVMe 落盘(裸盘直写,无文件系统)、GET 回 RDMA WRITE |
| **客户端** | bench / SDK | 本机 | 本仓 | 管理 GPU 显存/主机内存 buffer、发布 RDMA token、收发对象 |

> **架构一致性约束**:proxy 的控制面设计与 s3proxy 对齐——`upload_id`(multipart 会话句柄,等同 s3proxy UploadId)、`trace_id`(每 RPC 一个 snowflake,贯穿三层日志关联,等同 s3proxy TraceId)两个 id 两个作用域,client 不生成 id,只从响应读 `trace_id`。

### 2.2 控制面:proto + brpc

控制面全部由 `proto/control_plane.proto` 定义,brpc `cc_generic_services`。核心设计是 **"统一 PutObject 接口 + 通路枚举"**:

```proto
enum PutDataPath { PATH_NONE=0; PATH_GDS=1; PATH_RDMA=2; PATH_ALL=3; }
message ClientProxyPutRequest {
  string bucket, key; uint64 object_size; PutDataPath path;
  optional GdsDataSource  gds_source;   // PATH_GDS → cuObj RDMA token
  optional RdmaDataSource rdma_source;  // PATH_RDMA → hex(ip:port + rkey + addr + size)
}
```

- **每条链路一个独立 RPC**(`GdsPut` / `RdmaPut` / `UploadPartGds` / `UploadPartRdma` …),代码与测试隔离,v1 不做跨通路抽象。
- **Mode B**:所有控制面经 proxy,client 只与 proxy 交互;proxy 同步转发 backend。这是与"client 直连 backend"相对的集中调度模型,便于鉴权/限流/索引统一。
- **trace_id 贯穿三层**:proxy 生成 → 写进 dbgate session / ufile-ac 协议 sessionId / backend `[rdma-*]` 日志 → 响应回 client。定位任一对象的全链路只需 grep 一个 id。

### 2.3 数据面:GDS 与 RDMA 双通路

两条通路**数据源不同、协议不同、互不共享逻辑**,但都遵循"client 发布自描述 token → backend 反向连接并 RDMA READ"的 pull 模型:

| 维度 | **GDS 通路** | **RDMA 通路** |
|---|---|---|
| **数据源 buffer** | GPU 显存(device memory) | 主机内存(host memory) |
| **token 载体** | cuObj RDMA token(显存地址 + remote key 自描述串) | hex 串,含 listener ip:port + rkey + addr + size |
| **连接建立** | cuObj 链路(:18666) | RDMA CM(client 起 listener,backend 反向连接) |
| **搬运原语** | GDS RDMA READ backend 拉 GPU 显存 → NVMe | `ibv_post_send(RDMA_READ)` RC QP,backend 拉主机内存 → NVMe |
| **GET** | RDMA WRITE 回 client 显存 | RDMA WRITE 回 client 主机内存 |
| 省去的开销 | 连 `cudaMemcpy(host)` 都省,GPU 张量直出直入 | 省内核 TCP/`send`/`recv` 拷贝,host buffer 零拷贝 |
| **适用** | 训练 checkpoint 落盘 / KV cache 直取,GPU 侧零额外拷贝 | 主机侧大对象、非 CUDA 数据、跨进程共享内存 |

> 采用 pull(RDMA READ)而非 push(RDMA WRITE)模型的原因:client 的 buffer 地址/钥匙由 client 自描述发布,backend 解码后主动拉取,client 无需预先 `post_recv` 大缓冲,也无需向对端暴露自身内存布局——契合对象存储"client 多变、backend 固定"的不对称关系。

### 2.4 proxy 内部分层(依赖注入装配)

```
ProxyService (brpc Control 实现,只做 ClosureGuard + proto↔域对象转换 + Access 日志)
   ├── SinglePut     — 单步 GdsPut / RdmaPut
   ├── Multipart      — Create/UploadPart/Complete/Abort 会话(内存态,Mongo TTL 管 session 生命周期)
   └── GetObject      — Stat / GdsGet / RdmaGet
        │
        ├── UfileAcClient   — proxy→backend 二进制协议(ufile_ac_protocol),连接池 + SendAndRecv
        │     └── TcpConnection (连接池,backend_conn_pool_size)
        └── UploadIndex     — part 索引持久化(MongoUploadIndex,经 dbgate),GET 装配靠它
```

- **proxy 是无状态转发 + 会话持有**:multipart 会话在内存,MongoDB TTL 索引管过期,无后台线程;handler 并发安全。
- **连接池是关键调参点**:`backend_conn_pool_size` 决定 proxy→backend 的并发度,须与 `num_threads`(brpc worker)匹配,否则并发不足以打满 backend(实测 GDS 对该参数敏感,RDMA 钝感)。
- **`multipart_part_size` 是硬上限**:proxy 在 `multipart.cpp` 校验 part_size ≤ 该值;bench `--part-size` 须对齐,否则被拒。

### 2.5 backend (ufile-ac) 数据路径

backend 是独立仓 `ggds-compile-env/ufile-ac`,本仓不构建它,只通过 `ufile_ac_protocol.h`(从 backend `message.h` 拷贝对齐)与之通信。关键设计:

- **裸盘直写**:`/dev/nvme1n1` 整盘裸用(经 `osd/set01-m00-d00` 软链),**不经任何文件系统**,libaio 直提交 NVMe 队列,避免 page cache / inode 开销。
- **worker 线程池**:`[gds]` / `[rdma] worker_threads` 各 4,接 RDMA READ 完成后串行落盘;主 EventLoop 串行 dispatch。
- **MR pool**:每请求 `ibv_reg_mr` 7 ms 的瓶颈已用 MR pool(commit 4fdd47a,共享 PD)根治到 ~0,避免 per-req 注册开销。
- **mock 开关**:`mock_aio_write=1` 跳过落盘,只测数据搬运段(定位搬运瓶颈,排除 NVMe 干扰);`mock_rdma_read` / `mock_mode` 控 RDMA 侧 mock。性能调参常用 mock-on 隔离变量。

### 2.6 完整 PUT 数据流(分段上传)

```
client                                   proxy (:9100)                          backend (:24000 / :18666)
  │ CreateMultipartUpload ──brpc──►        │
  │ ◄── upload_id + trace_id ───────────── │
  │                                          │
  │ AcquireToken(device) / AcquireDescriptor │
  │ 起 RDMA CM listener, 发布 token         │
  │ UploadPart{Gds,Rdma}(token) ──brpc──►   │
  │                                          │ Validate(path/size/key)
  │                                          │ PutBlock{Gds,Rdma}(block_key, token)
  │                                          │ ──ufile_ac_protocol(TCP)──►  解码 token
  │                                          │                                RDMA CM 反连 client
  │ ◄════════════ RDMA READ (backend 拉 client buffer) ════════════════════
  │                                          │                                NVMe 裸盘落盘
  │                                          │ ◄── rsp(crc32c/etag)──────────
  │                                          │ WritePartIndex(Mongo) ──► dbgate
  │ ◄── trace_id + result ────────────────── │
  │ CompleteMultipartUpload ──brpc──►        │ 装配 part 索引 → 对象元数据
```

---

## 3 性能

### 3.1 测试环境

| 项 | 配置 |
|---|---|
| CPU | Xeon 8358P,128 核 |
| 内存 | 2.0 TiB |
| GPU | 8× A800-SXM4-80GB(GDS 用 GPU0) |
| RNIC | mlx5_2 → ens13f0np0,100Gb ConnectX-6 Dx |
| NVMe | `/dev/nvme1n1`,7.15 TiB 裸盘,TLC 稳态(smart-log Data Units Written 82.06 TB) |
| 部署 | client = proxy = backend 同机 192.168.1.198 |
| governor | performance |
| 最优配置 | part=4M / num_threads=8 / backend_conn_pool_size=16 / worker_threads=4 / client concurrency=16 |

> **重要前提**:重启 backend 后须充分预热 NVMe + RDMA-CM,否则冷态写吞吐跌至 ~1.7G,预热数轮后回到稳态值。所有数据为 SLC 缓存耗尽后的 TLC 稳态值。

### 3.2 GDS 通路性能

| 指标 | 值 | 说明 |
|---|---|---|
| **真写吞吐** | **~3.30 GiB/s** | 8 轮稳态均值 3290 MiB/s,±6% |
| **真读吞吐** | **~4.6 GiB/s** | conc=16,单调升(4171→4713) |
| **mock 仅搬运** | ~3.73 GiB/s | 跳过落盘,+13% vs 真写 |
| 单 part 端到端(真写) | ~12.3 ms | backend total ~8.42 ms |
| RDMA READ 4M | ~0.92 ms | 数据搬运段,非瓶颈 |
| aio_write(落盘) | ~2.3 ms | NVMe 写,真写瓶颈 |
| backend CPU(真写) | ~110%(≈1 核/128) | 富余 |
| NVMe util(真写) | 84%(3.3 GB/s 写) | 真写硬件瓶颈 |

**真写瓶颈** = NVMe 落盘(aio_write 2.3 ms + main_to_done 3.5 ms 等完成)+ worker 4 线程池排队(p95 9.2 ms)。数据搬运(RDMA READ 0.92 ms)健康,非瓶颈。

### 3.3 RDMA 通路性能

| 指标 | 值 | 说明 |
|---|---|---|
| **真写吞吐** | **~3.7 GiB/s** | 8 轮稳态均值 3742 MiB/s,±4%;reps=20 稳态 3813 |
| **真读吞吐** | **~4.7 GiB/s** | conc=16=4735,单调升 |
| **mock 仅搬运** | ~9.8–10.4 GiB/s | 跳过落盘,+162% vs 真写 |
| 单 part 端到端(真写) | ~11.4 ms | backend total ~4.85 ms |
| RDMA READ 4M | ~0.88 ms | 数据搬运段 |
| disk_wait(落盘) | ~3.4 ms(占 backend 70%) | NVMe 写,真写瓶颈 |
| backend CPU(真写) | ~92%(≈0.9 核/128) | 富余 |
| NVMe util(真写) | 99%(3.9 GB/s 写) | 近饱和 |

**真写瓶颈** = NVMe 落盘(disk_wait 3.4 ms,占 70%)+ proxy↔backend 往返+框架 ~5.2 ms。RDMA 提交 AIO 队列更深,故 NVMe util 99% 高于 GDS 84%、吞吐也更高(~3.7G vs ~3.3G)。

### 3.4 两通路对比与关键发现

| 维度 | GDS | RDMA |
|---|---|---|
| 真写 | ~3.30G | ~3.7G |
| 真读 | ~4.6G | ~4.7G |
| mock 搬运 | ~3.73G | ~9.8G |
| NVMe util(真写) | 84% | 99% |
| 数据源 | GPU 显存 | 主机内存 |

**关键发现**(均经多轮复测确认):

1. **NVMe 写带宽是真写共同天花板**:GDS ~3.3G / RDMA ~3.7G,差异来自 AIO 队列深度而非搬运能力。mock 下两路搬运能力差距巨大(GDS ~3.7G / RDMA ~9.8G),但被 NVMe 落盘统一拉到 ~3.3–3.7G。
2. **数据搬运(RDMA READ)本身极快**(~0.9 ms),两端性能都耗在搬运之外:真写卡 NVMe 落盘,纯搬运卡调度/框架往返与 worker 池长尾。
3. **同机部署下 RNIC 物理口 phy 计数全程 ≈0**:数据搬运走 mlx5 网卡内部 loopback/host 路径,**不经 100Gb 物理链路**。故 mock ~10G 非 RNIC 带宽上限,而是 host 侧搬运 dispatch 串行 + 往返的上限(backend CPU ~69% 仍有裕量)。**跨机部署**才会经 RNIC 物理链路、受 100Gb 约束——跨机结论不可由同机数据外推。
4. **真写与 mock 的最优 part 相反**:真写瓶颈在 NVMe 写延迟,小 part(2M/4M)并行度高更优;mock 瓶颈在固定开销摊薄,大 part(8M)更优。调参不可把 mock 结论直接搬到真写。
5. **CPU / 内存 / RNIC 均非瓶颈**:CPU 富余 >120 核、内存 2 TiB;唯一硬件瓶颈是 NVMe 写带宽。

### 3.5 资源占用(稳态,part=4M/nt=8/cp=16/conc=16)

| 资源 | GDS 真写 | RDMA 真写 | GDS mock | RDMA mock |
|---|---|---|---|---|
| backend CPU | ~110%(1 核) | ~92% | ~165% | ~69% |
| backend RSS | ~1174 MiB | ~1119 MiB | ~601 MiB | ~1003 MiB |
| NVMe util | 84%(3.3G 写) | 99%(3.9G 写) | 0.05%(跳过) | 0.05%(跳过) |
| GPU0 显存 | ~1.45 GB(RDMA READ 源) | N/A(host 内存面) | ~1.45 GB | N/A |
| RNIC phy | ≈0 | ≈0 | ≈0 | ≈0 |

### 3.6 已知瓶颈与优化方向

| 瓶颈 | 状态 | 证据 |
|---|---|---|
| `ibv_reg_mr` per-req 7 ms | **已根除**(MR pool,commit 4fdd47a) | regmr→0,稳态吞吐未变(7 ms 仅冷启动可见) |
| NVMe 写带宽 | 硬件天花板,真写主瓶颈 | GDS 84% / RDMA 99% util |
| worker 4 线程池排队 | 真写次要瓶颈,p95 9.2 ms | queue_wait 段 |
| host dispatch 串行 | mock 瓶颈(同机 loopback) | backend CPU ~69% 仍有裕量 |
| proxy↔backend 往返+框架 | 真写下 ~5 ms(mock ~0.7 ms) | recv − backend total 差值 |

**下一优化方向**(见 `MOONCAKE_OFFLOAD_SOLUTION.md`):在跨机部署下,RNIC 物理链路将成为约束,需评估 100Gb 带宽是否够用、是否需要 Mooncake 风格的 P2P/RDMA 直连 offload 进一步减少中间跳数。

---

## 附录:文档地图

| 文档 | 内容 |
|---|---|
| `README.md` | 编译、启动、跑 bench 的操作手册 |
| `docs/GDS_REAL_RW_REPORT.md` | GDS 真读写全表复测(2026-08-11) |
| `docs/RDMA_PERF_REPORT.md` | RDMA 性能全表复测(2026-08-11) |
| `docs/TUNING_DEEP_ANALYSIS.md` | part_size / num_threads / conn_pool 三轴调参 |
| `docs/MOONCAKE_OFFLOAD_SOLUTION.md` | 接入 Mooncake KVCache 冷层的方案与动机 |
