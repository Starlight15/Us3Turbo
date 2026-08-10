# GDS 比 RDMA 慢的原因分析

**日期**:2026-08-08(基于日志打点,二稿修正)
**对象**:最优配置(8M/16/16)真写下,GDS 与 RDMA 两条数据面通路的性能差距
**承接**:[REAL_WRITE_BOTTLENECK_ANALYSIS](REAL_WRITE_BOTTLENECK_ANALYSIS.md) 的 GDS 专项提炼;原始数据/拆解表见该文 §2–§8。

> 本文先讲结论与根因,再列已排除的常见误区(其中两条是一稿的定性错误,二稿已修正)。

---

## 1. 结论(先讲)

在最优配置(`part=8M / num_threads=16 / backend_conn_pool=16`)真写下:

| 指标 | RDMA | GDS | 比 |
|---|---|---|---|
| 分段吞吐 | 2727 MiB/s | 2300 MiB/s(wt=4 修复后) | GDS −16% |
| 每 part 墙钟(total_recv_to_response p50) | 13 ms | 38 ms(修复前)/ ~17 ms(修复后) | GDS 慢 |
| 单次 AIO 写延迟 p50 | 5272 µs | 6008 µs(wt=4)/ 11224 µs(wt=8) | — |
| cuobj/libibverbs RDMA-read p50 | 2186 µs | 2748 µs | GDS +25% |

**GDS 慢的根因(按影响排序)**:

1. **过提交 → NVMe 写队列争抢(头号根因,可修)**:GDS 默认 `worker_threads=8`,8 worker 把 8 路 AIO 同时推给同一块 NVMe,在途队列深度越过盘的最优区 → 单流 AIO 完成延迟膨胀(p50 11.2ms vs RDMA 5.3ms 的 **2.1×**),且尾部爆(>10ms 占 59.8% vs RDMA 31.4%)→ 聚合带宽反降。**修复**:把 `[gds] worker_threads` 8→4(与 RDMA 同在途数),per-AIO 11.2→6.0ms,吞吐 1894→2300(**+21–33%**,commit `b6ea675`)。
2. **cuobj RDMA-read 路径结构慢(次要,不可零改动)**:GDS 走 `cuMemObjGetRDMAToken` + cuobj 服务(18666)取 GPU 显存的 RDMA token 再 READ,比 RDMA 路径的 libibverbs 直连慢 25%(2748 vs 2186 µs,worker 段)。
3. **worker 队列尾部争抢**:8 worker 在 cuobj 读+AIO 完成回环里争抢,`queue_wait` p95 32.6ms / max 45.8ms(wt=8);wt=4 后消失。

---

## 2. 根因 1:过提交 → NVMe 写队列争抢(可修,已修)

### 机理

GDS 与 RDMA 的盘写都走**真异步 AIO**(`io_submit`+`eventfd`+非阻塞 `io_getevents`,见 `ustevent/src/libevent/aio_util.cc`),主 loop 不被盘写阻塞(打点证据:`done_cb_us ≈ aio_write_us`,gap p50=3µs,见 [REAL_WRITE_BOTTLENECK §2](REAL_WRITE_BOTTLENECK_ANALYSIS.md#2-决定性证据donecb--aiowritegap3µs))。所以吞吐上限由 **NVMe 聚合写带宽** 决定,而聚合带宽**随在途队列深度呈非单调**:在途太少→喂不饱盘(worker-bound);在途太多→单流完成延迟膨胀 + 尾部争抢→聚合反降。

GDS `worker_threads` = 在途 AIO 数(每 worker 端到端占一个 part,从 cuobj 读到 AIO 完成)。8 在途对这块 NVMe 过深 → 争抢。

### 扫描证据(`[gds] worker_threads ∈ {2,3,4,6,8}`,GDS multipart 8M conc32,见 [§8](REAL_WRITE_BOTTLENECK_ANALYSIS.md#8-gds-worker_threads-扫描已验证2026-08-08))

| gds wt | 吞吐 (MiB/s) | aio_write p50 (µs) | >10ms 尾部 | 机理 |
|---|---|---|---|---|
| 2 | 1594 | 4166 | 0% | 在途无争抢(per-AIO 最优),但 worker-bound |
| 3 | 1598 | 5016 | 低 | worker-bound:queue_wait p50=48ms(队列爆) |
| **4(峰)** | **2300–2368** | **6008–6100** | ~7% | 在途≈RDMA(wt=4),sweet spot |
| 6 | 1828 | 10710 | 高 | 争抢回升,per-AIO 翻倍 |
| 8(旧默认) | 1775–1894 | 11224–11399 | 60% | 过提交→聚合反降 |

**非单调曲线**(wt=4 处峰,wt=6/8 反降)就是"过提交适得其反"的铁证。争抢效应**独立于 SLC 态**:wt=4 fresh(25% SLC)per-AIO 6.0ms 仍远低于 wt=8 fresh(4% SLC)11.4ms;同 TLC 桶 2× 来自队列深度而非盘态。

### 修复

`[gds] worker_threads` 默认 8→4(commit `b6ea675`,零代码改动):吞吐 +21–33%、per-AIO −47%、争抢尾部 −53pp。详见 config 注释 + [§8](REAL_WRITE_BOTTLENECK_ANALYSIS.md#8-gds-worker_threads-扫描已验证2026-08-08)。

---

## 3. 根因 2:cuobj RDMA-read 路径结构慢(次要)

| 路径 | RDMA-read p50 | 机制 |
|---|---|---|
| RDMA(libibverbs 直连) | 2186 µs | `PostRead`+`PollOne` 直接走 RNIC,MR pool 命中(regmr=0) |
| GDS(cuobj token) | 2748 µs(+25%) | `cuMemObjGetRDMAToken(gpu_buffer)` 取显存 RDMA 可达地址+rkey,经 cuobj 服务(18666)中转,再 READ |

cuobj 路径多了 token 获取 + 服务中转,结构上慢 25%。这是 GDS 区别于 RDMA 的固有开销(GPU 显存直接 RDMA 的代价),**非零改动可消**(要改直连 cuobj,绕过 token 服务),且只占 per-part 38ms 的 ~7%,非主因。

---

## 4. 已排除的常见误区(重要)

一稿曾有两个定性错误,二稿已修正,列此避免再踩:

1. ~~GDS 每 part 2 次 AIO 写(DevDataHeader + data 分开)~~ → **错**。GDS 与 RDMA **都是 1 次 data AIO/part**(`ioContext.cc:412 WriteData` 单次 `SubmitWrite` 写 header+data 连续块;1024 part → 1024 aio_write 行)。GDS 慢不是因为多写一次。
2. ~~主 loop 被 AIO 写串行阻塞,占 per-part 68%~~ → **错**。AIO 写是**真异步**(`io_submit`+`eventfd`+非阻塞 `io_getevents`),主 loop 在盘写期间跑别的 part;`done_cb_us ≈ aio_write_us`(gap p50=3µs)证明完成侧主 loop CPU≈0。GDS 慢不是因为主 loop 被盘写占住。
3. ~~GDS buffer pool 争抢(4 个 16M buffer)~~ → **已证伪**(见 [GDS_POOL_OPTIMIZATION](GDS_POOL_OPTIMIZATION.md)):扫 max_per_class 1/2/4/8/32/64,峰值在 4(原值),扩容单调下降。pool 已最优,GDS↔RDMA 差距不在 pool。
4. ~~CPU 降频~~ → **已排除**:governor performance 真写反降(~1599)。
5. ~~MR pool~~ → **已排除**:关 MR pool 真写跌到 1451(更差)。

GDS 慢的真因就是 §1 的三条(过提交 + cuobj 读 + worker 队列尾部),首条已修。

---

## 5. 修复后剩余差距

wt=4 修复后:GDS 2300 vs RDMA 2727,仍低 16%。归因:

| 因素 | RDMA(wt=4) | GDS(wt=4) | 占差 |
|---|---|---|---|
| cuobj vs libibverbs 读 | 2186 µs | 2748 µs(+25%) | 主 |
| 单次 AIO 写 p50 | 5272 µs | 6008 µs | 次(在途数同,盘态差异残留) |
| 主 loop memcpy | 1715 µs | 954 µs | GDS 反而更小(非差因) |

剩余 16% 主要来自 cuobj 读路径(结构慢 25%,占读段),次来自 AIO 略高。要进一步缩小需 cuobj 直连(改代码,收益 ~7%)或 NVMe 提速(SLC 可用,两路同受益)。

---

## 6. 下一步(用户指导后)

1. **NVMe 写带宽(根本 cap,两路共同)**:~2700 是 TLC 直写上限,fio 直测裸盘写带宽+队列深度曲线对照;SLC 可用(盘空闲/trim)时两路同受益。
2. **cuobj 直连**(针对 GDS 剩余 16%):绕过 token 服务直连,收益 ~7%,代码改动。
3. ~~主 loop 并行化 / memcpy 零拷贝~~:AIO 已异步、主 loop 不被盘写阻塞、memcpy 非 cap(GDS 954µs 提交上限 ~8400 MB/s >> 实测),收益低;仅 NVMe 回 SLC 速时 memcpy 才可能成 cap。

---

## 附录:关键源码定位

- `gds_service.cc:115` `for (i<worker_threads)` — 起 N worker(=在途 AIO 数)
- `gds_service.cc:270` `[gds-timing] rdma_read`(cuobj 读段)
- `ac_server.cc:1514` `PutFromGds` → `WriteData`(主 loop,1 次 SubmitWrite)
- `ioContext.cc:412` `WriteData`→`SubmitWrite(...,WriteDeviceDone,this)`(异步)
- `ustevent/src/libevent/aio_util.cc:50/97` `SubmitWrite`/`SomethingDone`(eventfd 异步)
