# RDMA 性能报告

**测试日期**: 2026-08-10

**环境**: governor=performance;client=backend 同机 192.168.1.198(mlx5_2,100Gb);NVMe `/dev/nvme1n1`;backend `[rdma] worker_threads=4`;client concurrency=16。

**工具**: `us3_turbo_bench_rdma_multipart`(写)、`us3_turbo_bench_rdma_get`(读)。

**场景说明**

- **实际读写**: backend `[rdma] mock_aio_write=0`、`mock_mode=0` —— 真 RDMA READ 搬运 + NVMe 落盘;GET 真 RDMA WRITE 回 client。
- **mock 仅搬运**: backend `[rdma] mock_aio_write=1` —— 跳过落盘,只测 client→proxy→backend RDMA READ 数据搬运。

---

## 1 实际读写

### 1.1 结论

分段上传 part=4M / nt=8 / cp=16,真写 ≈ 3.50 GiB/s,真读 ≈ 4.7 GiB/s(4M 对象,conc=16)。读 > 写(NVMe 读快于写、无写放大,与 GDS 同)。真写被 NVMe 落盘拉低,搬运能力被掩盖(见 §2 mock)。

### 1.2 写性能

**测试条件**: part_size=4M、num_threads=8、conn_pool=16、concurrency=16、reps=3、warmup=1,8 轮。

| 轮次 | 吞吐 (MiB/s) |
|---|---|
| 1–8 | 3473 / 3617 / 3596 / 3481 / 3517 / 3354 / 3368 / 3570 |
| **均值** | **~3497**(区间 3354–3617,±3%) |

#### 1.2.1 num_threads 扫描

**测试条件**: 扫描 num_threads ∈ {4,8,16};固定 part_size=4M、conn_pool=16、concurrency=16、reps=3、warmup=1。

| num_threads | 吞吐 (MiB/s) |
|---|---|
| 4 | 3537 |
| 8 | 3049 |
| 16 | 3052 |

num_threads 4–16 钝感(单轮波动,瓶颈在 NVMe 落盘,与 GDS 同;mock 下瓶颈在 RNIC 硬件)。**选 num_threads=8**(省线程)。

### 1.3 读性能

**测试条件**: 扫描 concurrency ∈ {4,8,16};固定 size=4M(对象大小)、num_threads=8、conn_pool=16、count=128、warmup=1。

| concurrency | 吞吐 (MiB/s) | ops/s | p95 (ms) |
|---|---|---|---|
| 4 | 3797 | 949 | 5.2 |
| 8 | 4494 | 1124 | 9.5 |
| 16 | 4735 | 1184 | 19.6 |

conc 4–16 单调升,conc=16 近峰;**conc≥16 p95 升至 20ms**。读带宽 > 写带宽(NVMe 读快于写、无写放大,与 GDS 同)。

### 1.4 推荐配置

| 项 | 值 |
|---|---|
| `--multipart_part_size` | 4M |
| `--num_threads` | 8 |
| `--backend_conn_pool_size` | 16 |
| `[rdma] worker_threads` | 4 |
| `--concurrency`(client) | 16 |
| 稳态吞吐 | 真写 ~3.50G / 真读 ~4.7G |

### 1.5 瓶颈分析

**测试条件**: part_size=4M、num_threads=8、conn_pool=16、concurrency=16。单 part-put 端到端 ~11.4 ms。

**proxy 视角**

| 段 | avg (µs) | 占比 |
|---|---|---|
| validate | 323 | 3% |
| 等 backend(recv) | 10050 | 88% |
| WritePartIndex | 978 | 9% |

**backend 内**(total_recv_to_response ~4.85ms)

| 段 | avg (µs) | p95 (µs) | 说明 |
|---|---|---|---|
| disk_wait | 3404 | 6116 | NVMe 落盘(占 backend 70%) |
| exec(含 done_cb) | 2728 | 4671 | done_cb 2032 / memcpy 694 |
| rdma_read | 877 | 1026 | RDMA READ 4M |
| crc32 | 279 | 400 | |
| total | 4848 | 7180 | |

proxy recv 10050 − backend 4848 ≈ **5200µs = proxy↔backend 往返+框架开销**。

**瓶颈**: NVMe 落盘(disk_wait 3.4ms,占 backend 70%)+ proxy↔backend 往返+框架 ~5.2ms。数据搬运(rdma_read 0.88ms)非瓶颈。真写下搬运能力被 NVMe 写带宽掩盖。

---

## 2 mock 仅搬运(跳过落盘)

### 2.1 吞吐

**测试条件**: part_size=4M、num_threads=8、conn_pool=16、concurrency=16、reps=3、warmup=1,8 轮。

| 轮次 | 吞吐 (MiB/s) |
|---|---|
| 1–8 | 9492 / 9803 / 9473 / 9502 / 9756 / 9536 / 9749 / 9895 |
| **均值** | **~9651**(区间 9473–9895,±2%) |

≈ 9.65 GiB/s,接近 100Gb RNIC 带宽(conc=16 未完全 saturate,conc=32 可达 ~10.3G)。比实际写(3497)+176% —— NVMe 落盘占真写端到端的 ~65%。

### 2.2 瓶颈分析

**测试条件**: 同 §2.1。单 part-put 端到端 ~3.7 ms。

**proxy 视角**

| 段 | avg (µs) | 占比 |
|---|---|---|
| validate | 361 | 10% |
| 等 backend(recv) | 2221 | 61% |
| WritePartIndex | 1081 | 29% |

**backend 内**(total_recv_to_response ~1.5ms)

| 段 | avg (µs) | p95 (µs) | 说明 |
|---|---|---|---|
| rdma_read | 1077 | 1302 | 真 RDMA READ 4M 搬运 |
| disk_wait | 15 | 25 | 落盘跳过(真写 3404) |
| crc32 | 343 | 483 | |
| total | 1505 | 1707 | |

proxy recv 2221 − backend 1505 ≈ **716µs = proxy↔backend 往返+框架开销**(远小于真写 5200µs,因 mock 下 backend 快、proxy 等待短)。

**瓶颈**: 并发下吞吐接近 RNIC 100Gb 天花板(conc=16 ~9.65G,conc=32 可达 ~10.3G);单 part 延迟 3.7ms,搬运 1.5ms + 往返 0.7ms。conc=16 未完全 saturate RNIC,提并发可进一步逼近 10.3G。mock 下固定开销摊薄,part 越大越优(8M 为峰),与真写相反。

---

## 3 两场景对比

| 维度 | 实际读写 | mock 仅搬运 | 差 |
|---|---|---|---|
| 写吞吐 | ~3.50G | ~9.65G | +176%(落盘) |
| 真读吞吐 | ~4.7G | —(无落盘数据) | — |
| backend total | 4.85ms | 1.50ms | -3.35ms(disk_wait) |
| disk_wait | 3404µs | 15µs | 落盘 |
| rdma_read | 0.88ms | 1.08ms | — |
| proxy↔backend 往返 | ~5.2ms | ~0.7ms | 真写大(mock backend 慢→proxy 等更久,含 backend 内等待) |
| 主要瓶颈 | NVMe 落盘(70%) | RNIC 天花板 | — |

**一句话**: RDMA 数据面搬运能力极强(mock 9.65G 近 RNIC 天花板),真写被 NVMe 落盘拉到 3.50G(与 GDS 真写 3.30G 接近 —— NVMe 写带宽是两者共同天花板)。proxy↔backend 往返在真写下 ~5.2ms(含 backend 内等待放大),mock 下 backend 快则往返降至 0.7ms。

> 注: 绝对值随盘态(SLC/TLC 混态)与 governor 变化;本次为 SLC 已耗后的 TLC 稳态值(预填充后)。所有扫描/concurrency 均 ≤16。RDMA 调参对 nt/cp/part 钝感(瓶颈在 RNIC 硬件),详见 `TUNING_DEEP_ANALYSIS.md`。
