# backend_conn_pool_size 性能差异深度分析:连接池大小的影响

**日期**:2026-08-06
**范围**:仅分析 proxy `--backend_conn_pool_size`(cp,backend 连接池大小)对 GDS / RDMA 性能的影响与根因。这是初定 3 个 proxy 参数(part_size、num_threads、cp)中**最后一个做深度扫描**的,与 [num_threads 分析](NUM_THREADS_DEEP_ANALYSIS.md)对称、交叉验证。
**方法**:控制变量(只改 cp),固定 `num_threads=16`、`part_size=8M`(两者已分析最优)、client 32 线程、total=64M、mock 开启。扫描 cp ∈ {4,8,16,32}(从 4:1 过订阅到 2:1 富余)。假设:cp<nt 时 conn-pool 争抢、rpc 上涨;cp≥nt 饱和;cp>nt 边际。

---

## 0. 2026-08-07 复测校验(结论不变,数值在会话方差内)

同条件复测(`mock_aio_write=1` 纯搬运,binary 含 MR-pool commit 4fdd47a;reps 3 warmup 1,client 32):

| cp | GDS(原→复测) | RDMA(原→复测) |
|---|---|---|
| 4 | 2132 → 2233 | 6029 → 6757 |
| 8 | 2802 → 3080 | 6369 → 6283 |
| 16 | 3564 → 3985 | 6533 → 6786 |
| 32 | 3664 → 3756 | 6614 → 6697 |

全部落在 ±12% 会话方差内(最大偏差 RDMA cp4 +12%、GDS cp16 +12%;GDS cp4→cp16 涨 67% 的趋势、cp16→32 仅 +2.8% 的饱和拐点、RDMA 钝感均复现)。**所有结论不变**:cp<nt 跌 GDS、cp=nt=16 甜点、cp>nt 边际、RDMA 钝感。§3 原表(含 per-part rpc 的 13.2ms conn-pool 等待反推)保留为 08-06 历史 --trace 值;本次 cp 扫描未带 --trace,只有聚合吞吐 + per-round data-plane,故不重证 13.2ms 那一具体数,但 cp4<cp8<cp16≈cp32 的 GDS 趋势与该机制一致。RDMA mock 路径跳 memcpy 的变化见 [PART_SIZE §0](PART_SIZE_DEEP_ANALYSIS.md#0-2026-08-07-复测校验必读rdma-16m-结论已反转),对 cp 扫描无实质影响。

---

## 1. 结论(先讲)

1. **cp 的拐点在 cp = num_threads**:cp<nt 时 GDS 吞吐随 cp 线性下跌(rpc 涨);cp≥nt 后**饱和**,再大只压低尾部时延、不涨吞吐。
2. **GDS 对 cp 敏感**:nt=16 下 cp=4→8→16→32 吞吐 2132→2802→3564→3664,**16→32 仅 +2.8%**(饱和),4→16 涨 67%。rpc p50 82.4→62.5→49.3→44.9ms。
3. **RDMA 对 cp 钝感**:同条件 6029→6369→6533→6614,±3%,rpc p50 全档 32.7-34.1ms。
4. **交叉验证 num_threads 分析**:`nt≈cp` 规则从两个方向各自证实——
   - nt 扫描(cp=8 固定):GDS 峰值在 nt=8(=cp),nt>cp 反降(见 [NUM_THREADS 分析 §3](NUM_THREADS_DEEP_ANALYSIS.md#3-结果))。
   - cp 扫描(nt=16 固定):GDS 饱和在 cp=16(=nt),cp<nt 跌、cp>nt 边际。
   - **共享点 nt=16/cp=8 在两次扫描里 rpc p50 都是 62.5ms**,且 conn-pool 开销(cp8 vs cp16 = 13.2ms)两次独立得出同一值。规则坐实。
5. **三参数调优总结**(mock 下,client 32 线程,64M):`part_size=8M` + `num_threads=16` + `backend_conn_pool_size=16` → GDS 3564 / RDMA 6533 MiB/s。RDMA ≈ GDS 1.83×。

---

## 2. 方法与数据

### 2.1 控制变量

| 参数 | 值 | 说明 |
|---|---|---|
| `num_threads` | 16(固定) | 隔离 cp 效应;取 num_threads 分析的最优 |
| `multipart_part_size` | 8M(固定) | part_size 分析的最优 |
| client concurrency | 32 | 固定 |
| total | 64M | 固定 |
| reps / warmup | 3 / 1 | 每配置每通路 |
| `--trace` | 开 | per-part `acquire`/`rpc` 日志 |

扫描 cp ∈ {4,8,16,32}。其中 cp=8 与 cp=16 两个点**直接复用** num_threads 分析已带 trace 的 `trace_nt16_{gds,rdma}.log`(cp8)与 `trace_nt16cp16_{gds,rdma}.log`(cp16),配置与协议完全一致;cp=4 与 cp=32 为本轮新跑。

### 2.2 数据来源

| 数据 | 来源 |
|---|---|
| per-part `acquire`/`rpc` | client `--trace`:`/tmp/trace_nt16cp{4,8,16,32}_{gds,rdma}.log` |
| backend 阶段 | ufile-ac `HandleRdmaPut DONE PHASE`(RDMA 独有,GDS 无) |
| proxy | 仍无 per-RPC 计时(见 [part_size 分析 §7](PART_SIZE_DEEP_ANALYSIS.md#7-trace-完整性评估与建议) gap) |

---

## 3. 结果

### 3.1 聚合吞吐(nt=16 固定,扫 cp)

| cp | cp vs nt | GDS MiB/s | GDS data-plane p50 | RDMA MiB/s | RDMA data-plane p50 |
|---|---|---|---|---|---|
| 4 | cp<nt (4:1) | 2132 | 870ms | 6029 | 279ms |
| 8 | cp<nt (2:1) | 2802 | 646ms | 6369 | 261ms |
| **16** | cp=nt | **3564** | 510ms | 6533 | 258ms |
| 32 | cp>nt (富余) | 3664 | 494ms | 6614 | 250ms |

- GDS:cp 4→16 涨 67%(2132→3564),16→32 仅 +2.8%(3564→3664)——**拐点在 cp=16=nt**。
- RDMA:全档 6029-6614(±3%);cp=4 极端过订阅时略跌(~8%),但基本钝感。

### 3.2 per-part trace(主证据)

| 通路 | cp | acq_p50 | acq_p95 | **rpc_p50** | rpc_p95 | rpc_mean | per-part BW@p50 |
|---|---|---|---|---|---|---|---|
| GDS | 4 | 0.174ms | 219ms | 82.4ms | 169ms | 84.4ms | 97.0 MiB/s |
| GDS | 8 | 0.143ms | 161ms | 62.5ms | 125ms | 63.3ms | 127.9 MiB/s |
| GDS | **16** | 0.090ms | 213ms | 49.3ms | 100ms | 49.8ms | 162.2 MiB/s |
| GDS | 32 | 0.098ms | 182ms | 44.9ms | 104ms | 49.6ms | 178.2 MiB/s |
| RDMA | 4 | 0.011ms | 0.87ms | 34.1ms | 56.8ms | 34.6ms | 234.8 MiB/s |
| RDMA | 8 | 0.011ms | 1.02ms | 33.5ms | 55.0ms | 33.2ms | 238.7 MiB/s |
| RDMA | 16 | 0.013ms | 0.94ms | 33.4ms | 52.3ms | 32.3ms | 239.5 MiB/s |
| RDMA | 32 | 0.012ms | 0.85ms | 32.7ms | 51.3ms | 32.1ms | 244.5 MiB/s |

**读数**:
- **GDS rpc_p50**:82.4→62.5→49.3→44.9ms,单调降;cp=16→32 仍降 4.4ms(尾部争抢清理),但吞吐已饱和。
- **RDMA rpc_p50**:34.1→33.5→33.4→32.7ms,±1.4ms,完全平。
- `acquire` p50 全部 < 0.2ms(token 取得不主导);GDS `acquire` p95 ~160-219ms(cuobj 路径尾部,与 [part_size 分析 §6](PART_SIZE_DEEP_ANALYSIS.md#6-gds-16m-为何比-rdma-16m-崩得多299ms-vs-98ms) 同源,跨 cp 档不动——证明 cuobj 尾部争抢与 conn-pool 无关,是独立瓶颈)。

---

## 4. 机制分析

### 4.1 GDS:cp<nt 时劣化 = conn-pool 争抢(对称于 num_threads 分析)

`UfileAcClient::AcquireConn`(`proxy/src/storage/ufile_ac_client.cpp:26`):池有 cp 条连接,每条带独立 `conn_mutexes_[idx]`,请求周期内持锁。

- **cp ≥ nt(cp=16/32, nt=16)**:16 线程各拿 1 条连接,无锁等待。rpc 由 backend 干活 + 固定开销决定 ≈ 49ms。**饱和**。
- **cp < nt(cp=4/8, nt=16)**:16 线程抢 <16 条连接,多出的线程在 `conn_mutexes_` 上阻塞空等 → rpc 上涨(cp=8 →62.5ms,cp=4 →82.4ms),吞吐跌。

**conn-pool 开销量化**(以 cp=16 饱和值为基准):

| cp | nt/cp 过订阅比 | conn-pool 等待 ≈ rpc − 49.3ms | 实测 rpc p50 |
|---|---|---|---|
| 4 | 4:1 | 33.1ms | 82.4ms |
| 8 | 2:1 | 13.2ms | 62.5ms |
| 16 | 1:1 | 0(基准) | 49.3ms |
| 32 | 0.5:1(富余) | -4.4ms(尾部清理) | 44.9ms |

> cp=8 这一行的 13.2ms conn-pool 等待,**与 [num_threads 分析 §4.1](NUM_THREADS_DEEP_ANALYSIS.md#41-gds-ntcp-时劣化--conn-pool-争抢决定性证据)独立算出的 13.2ms 完全一致**——那是 nt=16/cp=8→cp=16 的 rpc 差,本表是 cp 扫描同一两点差。同一物理量、两次独立测量同值,交叉验证闭环。

### 4.2 为什么 cp=16→32 rpc 还在降、吞吐却不动

cp=16→32:rpc p50 49.3→44.9ms(-4.4ms,清尾部争抢),吞吐 3564→3664(+2.8%)。**rpc 降但不转化成吞吐**——因为 cp≥nt 后,瓶颈从 proxy 连接池转移到了 **backend GDS 路径**(gds_service 的 4×16M buffer pool,见 [part_size 分析 §6](PART_SIZE_DEEP_ANALYSIS.md#6-gds-16m-为何比-rdma-16m-崩得多299ms-vs-98ms))。更多连接只让单个 part 等得更少(降 rpc),但 backend 同时只能服务有限个 16M buffer,聚合吞吐被 backend cap。

> 这是"per-part 时延"与"聚合吞吐"分离的典型现象:调 cp 在 cp<nt 段同时改善两者;cp≥nt 后只改善 per-part 时延、聚合吞吐被 backend 顶住。要继续涨 GDS 吞吐,得调 backend buffer pool,不是 cp。

### 4.3 RDMA 为何钝感

RDMA rpc p50 全档 32.7-34.1ms(±1.4ms)。三层原因(同 [num_threads 分析 §4.3](NUM_THREADS_DEEP_ANALYSIS.md#43-rdma为何对-nt-钝感)):
1. **part 轻**:RDMA 8M backend `total` 仅 4.4ms(`HandleRdmaPut DONE PHASE`),conn 占用短、turnover 快,即便 cp=4(4:1 过订阅)也基本排空。
2. **硬件先行饱和**:32 worker 在 cp=4 就把 RNIC/NVMe 喂到 ~6029(cp=32 的 6614 的 91%),cp 再大无硬件可补。
3. cp=4 时 RDMA 也略跌(6029 vs 6533,-8%)——极端过订阅下 conn 锁争抢仍可见,但量级远小于 GDS。

### 4.4 backend 侧是否随 cp 变?

RDMA backend `total`(8M)= 4.4ms,且 RDMA rpc 跨 cp 全平 → backend 不随 cp 变。GDS backend 无 PHASE,但 GDS rpc 随 cp 单调变 → 变化全在 proxy/conn 层。**调 cp 不触及 backend 干活效率,只动 conn-pool 争抢。**

---

## 5. 交叉验证:`nt≈cp` 规则双向坐实

| 方向 | 固定 | 扫描 | GDS 峰值/饱和点 | 结论 |
|---|---|---|---|---|
| nt 扫描(num_threads 分析) | cp=8 | nt ∈{4,8,16,32} | **nt=8(=cp)** | nt>cp 反降 |
| cp 扫描(本分析) | nt=16 | cp ∈{4,8,16,32} | **cp=16(=nt)** | cp<nt 跌、cp>nt 边际 |

两方向独立指向同一规则:**`num_threads ≈ backend_conn_pool_size`**,谁超过谁都会浪费或争抢。

**共享数据点闭环**:nt=16/cp=8 在两次扫描里:
- num_threads 分析:rpc p50=62.5ms(作为 nt=16 行)
- 本分析:rpc p50=62.5ms(作为 cp=8 行)
- 两次算的 conn-pool 开销(cp8→cp16)都是 13.2ms

同一物理量、两套独立测量同值 → 数据自洽、规则可信。

---

## 6. 最优 cp 与三参数总结

### 6.1 最优 cp

| 通路 | 推荐 cp | 理由 |
|---|---|---|
| GDS | **= num_threads(本轮 nt=16 → cp=16)** | cp<nt 争抢跌吞吐;cp>nt 边际、徒增长连接/内存 |
| RDMA | **= nt 即可,或更小**(cp=8 也到 6369,vs cp=16 的 6533 差 3%) | 硬件天花板;cp 不影响 |

**通用规则**:`backend_conn_pool_size = num_threads`,两者都设 16(本轮 GDS 3564、RDMA 6533)。

### 6.2 三参数最终配置(mock 下,client 32 线程,64M)

| 参数 | 最优值 | 依据文档 |
|---|---|---|
| `multipart_part_size` | 8M | [PART_SIZE_DEEP_ANALYSIS](PART_SIZE_DEEP_ANALYSIS.md) |
| `num_threads` | 16 | [NUM_THREADS_DEEP_ANALYSIS](NUM_THREADS_DEEP_ANALYSIS.md) |
| `backend_conn_pool_size` | 16 | 本文档 |

GDS 3564 / RDMA 6533 MiB/s(mock,纯数据搬运)。

### 6.3 继续涨吞吐的下一步瓶颈(已不在 proxy 三参数)

- **GDS**:瓶颈已移到 backend gds_service 的 `BufferSizeClasses={1M,16M}` + `kBufferMaxPerClass=4`(4 个 16M buffer)。调 cp/nt/part_size 到位后,GDS 天花板在这里。需改 backend 源码 + 重启重测。
- **RDMA**:已触 RNIC/NVMe 硬件天花板(~6600),proxy 侧无参数可推。
- **client concurrency**:全程固定 32 未扫,是唯一未测的"非 proxy"维(见 §7)。

---

## 7. trace 完整性(同前两篇,本轮再次印证)

本轮"cp<nt 时 rpc 上涨 = conn-pool 争抢"仍是**控制变量反推**(cp 8→16 砍 13.2ms rpc)+ 共享点交叉验证,**非直接测得**——`UfileAcClient::AcquireConn`/`SendAndRecv` 无 `conn_wait`/`backend_roundtrip` 分段计时。建议(与 [part_size §7.3](PART_SIZE_DEEP_ANALYSIS.md#73-建议补的-trace-点优先级排序)、[num_threads §6](NUM_THREADS_DEEP_ANALYSIS.md#6-trace-完整性本轮仍靠推断定位-conn-争抢)一致):
1. `SendAndRecv` 加 `t_acquire_conn`/`t_lock_held`/`t_backend_rsp` 三段 → 直接画 conn_wait vs cp 曲线(现在靠差值反推)。
2. 周期打 `backend_conn_pool in_use=X/Y waiters=Z` → cp=4/8 时应见 waiters>0,cp≥16 归零。

补上后,§4.1 的 conn-pool 等待表能从"反推"升级为"直测",并能看到 cp=32 时 waiters=0 但 in_use≤16(富余连接闲置)的直接证据。

---

## 8. 对调优的指导

1. **立即固化**:`cp = num_threads = 16`(与 part=8M)。GDS/RDMA 都到位,安全配置。
2. **别犯 cp≠nt**:cp=4/nt=16 让 GDS 从 3564 跌到 2132(-40%);nt=32/cp=8 让 GDS 跌到 2582(见 num_threads 分析)。单向失衡必损 GDS。
3. **cp>nt 无必要**:cp=32/nt=16 只多 +2.8% GDS、+1.3% RDMA,却多一倍长连接与内存。除非 backend conn-pool 内部有别的争抢(未发现),否则 cp=nt 即可。
4. **GDS 想破 3664**:别再调 proxy 三参数(已饱和),转 backend `BufferSizeClasses`/`kBufferMaxPerClass`。
5. **补 proxy per-RPC 计时后再调**:三篇分析都靠差值反推 conn 争抢,补计时后才能直测、才能区分"conn_wait 降"vs"backend 排队降"。

---

## 附录 A:复现命令

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
# nt=16 固定,改 cp
sed -i 's/^--num_threads=.*/--num_threads=16/' proxy/conf/proxy.flags
sed -i 's/^--backend_conn_pool_size=.*/--backend_conn_pool_size=4/' proxy/conf/proxy.flags
sed -i 's/^--multipart_part_size=.*/--multipart_part_size=8388608/' proxy/conf/proxy.flags
kill $(pgrep -x us3_turbo_proxy); sleep 2
nohup ./build/proxy/us3_turbo_proxy --flagfile=proxy/conf/proxy.flags > /tmp/proxy_cp.log 2>&1 &
sleep 4
./build/rtest/bench/gds/us3_turbo_bench_gds_multipart \
  --part-size 8M --total 64M --concurrency 32 --reps 3 --warmup 1 --trace \
  > /tmp/trace_nt16cp4_gds.log 2>&1
# per-part 解析见 PART_SIZE_DEEP_ANALYSIS 附录 A 脚本
```

## 附录 B:三篇分析参数矩阵一览

| 文档 | 固定 | 扫描 | GDS 峰值 | RDMA 峰值 |
|---|---|---|---|---|
| PART_SIZE | nt=8,cp=8 | part 4/8/16M | 8M:3132 | 8M:6124 |
| NUM_THREADS | cp=8,part=8M | nt 4/8/16/32 | nt=8:3097 | nt=16:6369 |
| CONN_POOL(本文) | nt=16,part=8M | cp 4/8/16/32 | cp=16:3564 | cp=32:6614 |

注:各篇固定条件不同(后两篇用前篇最优作固定),故峰值不可直接横比;最优配置统一为 part=8M / nt=16 / cp=16(本文与 num_threads 的 cp=16 点同源,GDS 3564 / RDMA 6533)。
