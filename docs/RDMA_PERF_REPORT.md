# RDMA 性能报告

**测试日期**: 2026-08-10

**环境**: governor=performance;client=backend 同机 192.168.1.198(mlx5_2,100Gb);NVMe `/dev/nvme1n1`;backend `[rdma] worker_threads=4`;client 32 线程。

**工具**: `us3_turbo_bench_rdma_multipart`(写)、`us3_turbo_bench_rdma_get`(读)。

**场景说明**

- **实际读写**: backend `[rdma] mock_aio_write=0`、`mock_mode=0` —— 真 RDMA READ 搬运 + NVMe 落盘;GET 真 RDMA WRITE 回 client。
- **mock 仅搬运**: backend `[rdma] mock_aio_write=1` —— 跳过落盘,只测 client→proxy→backend RDMA READ 数据搬运。

---

## 1 实际读写

### 1.1 结论

分段上传 part=4M / nt=8 / cp=32,真写 ≈ 3.56 GiB/s,真读 ≈ 5.1 GiB/s(4M 对象,conc=16)。读 > 写(NVMe 读快于写、无写放大,与 GDS 同)。真写被 NVMe 落盘拉低,搬运能力被掩盖(见 §2 mock)。

### 1.2 写性能

**测试条件**: part_size=4M、num_threads=8、conn_pool=32、concurrency=32、reps=3、warmup=1,8 轮。

| 轮次 | 吞吐 (MiB/s) |
|---|---|
| 1–8 | 3501 / 3509 / 3627 / 3601 / 3447 / 3583 / 3619 / 3583 |
| **均值** | **~3559**(区间 3447–3627,±3%) |

#### 1.2.1 num_threads 扫描

**测试条件**: 扫描 num_threads ∈ {4,8,16};固定 part_size=4M、conn_pool=32、concurrency=32、reps=3、warmup=1。

| num_threads | 吞吐 (MiB/s) |
|---|---|
| 4 | 3674 |
| 8 | 3524 |
| 16 | 3529 |

num_threads 4–16 钝感(±2%,瓶颈在 NVMe 落盘,与 GDS 同;mock 下瓶颈在 RNIC 硬件)。**选 num_threads=8**(省线程)。

### 1.3 读性能

**测试条件**: 扫描 concurrency ∈ {4,8,16};固定 size=4M(对象大小)、num_threads=8、conn_pool=32、count=128、warmup=1。

| concurrency | 吞吐 (MiB/s) | ops/s | p95 (ms) |
|---|---|---|---|
| 4 | 4168 | 1042 | 5.4 |
| 8 | 4757 | 1189 | 8.9 |
| 16 | 5136 | 1284 | 18.6 |

conc 4–16 单调升(4.2→5.1 GiB/s),conc=16 近峰;**conc≥16 p95 升至 18ms**。读带宽 > 写带宽(NVMe 读快于写、无写放大,与 GDS 同)。

### 1.4 推荐配置

| 项 | 值 |
|---|---|
| `--multipart_part_size` | 4M |
| `--num_threads` | 8 |
| `--backend_conn_pool_size` | 32 |
| `[rdma] worker_threads` | 4 |
| 稳态吞吐 | 真写 ~3.56G / 真读 ~5.1G |

### 1.5 瓶颈分析

**测试条件**: part_size=4M、num_threads=8、conn_pool=32、concurrency=32。单 part-put 端到端 ~9.9 ms。

**proxy 视角**

| 段 | avg (µs) | 占比 |
|---|---|---|
| validate | 316 | 3% |
| 等 backend(recv) | 8597 | 87% |
| WritePartIndex | 972 | 10% |

**backend 内**(total_recv_to_response ~4.48ms)

| 段 | avg (µs) | p95 (µs) | 说明 |
|---|---|---|---|
| disk_wait | 3296 | 5364 | NVMe 落盘(占 backend 74%) |
| rdma_read | 809 | 944 | RDMA READ 4M |
| exec(含 memcpy/done_cb) | 2789 | 4318 | memcpy 615 / done_cb 2172 |
| crc32 | 329 | 438 | |
| post | 501 | 1143 | |
| total | 4476 | 6499 | |

proxy recv 8597 − backend 4476 ≈ **4121µs = proxy↔backend 往返+框架开销**。

**瓶颈**: NVMe 落盘(disk_wait 3.3ms,占 backend 74%)+ proxy↔backend 往返+框架开销 ~4.1ms。数据搬运(rdma_read 0.81ms)非瓶颈。真写下搬运能力被 NVMe 写带宽掩盖。

---

## 2 mock 仅搬运(跳过落盘)

### 2.1 吞吐

**测试条件**: part_size=4M、num_threads=8、conn_pool=32、concurrency=32、reps=3、warmup=1,8 轮。

| 轮次 | 吞吐 (MiB/s) |
|---|---|
| 1–8 | 10015 / 10384 / 10404 / 10129 / 10420 / 10388 / 10175 / 10393 |
| **均值** | **~10288**(区间 10015–10420,±2%) |

≈ 10 GiB/s,**即 100Gb RNIC 硬件带宽天花板**。比实际写(3559)+189% —— NVMe 落盘占真写端到端的 ~65%。

### 2.2 瓶颈分析

**测试条件**: part_size=4M、num_threads=8、conn_pool=32、concurrency=32。单 part-put 端到端 ~6.7 ms。

**proxy 视角**

| 段 | avg (µs) | 占比 |
|---|---|---|
| validate | 341 | 5% |
| 等 backend(recv) | 5272 | 79% |
| WritePartIndex | 1025 | 15% |

**backend 内**(total_recv_to_response ~1.40ms)

| 段 | avg (µs) | p95 (µs) | 说明 |
|---|---|---|---|
| rdma_read | 1146 | 1333 | 真 RDMA READ 4M 搬运 |
| disk_wait | 15 | 22 | 落盘跳过(真写 3296) |
| crc32 | 194 | 232 | |
| total | 1401 | 1560 | |

proxy recv 5272 − backend 1401 ≈ **3871µs = proxy↔backend 往返+框架开销**(与真写 4121µs 同量级,与落盘无关)。

**瓶颈**: 并发下吞吐撞 RNIC 100Gb 天花板(~10G);单 part 延迟 6.7ms 主要由 proxy↔backend 往返 ~3.9ms 占据(58%),搬运本身仅 1.4ms。并发掩盖了往返延迟,吞吐受 RNIC 带宽封顶。

---

## 3 两场景对比

| 维度 | 实际读写 | mock 仅搬运 | 差 |
|---|---|---|---|
| 写吞吐 | ~3.56G | ~10.3G | +189%(落盘) |
| 真读吞吐 | ~5.1G | —(无落盘数据) | — |
| backend total | 4.48ms | 1.40ms | -3.1ms(disk_wait) |
| disk_wait | 3296µs | 15µs | 落盘 |
| rdma_read | 0.81ms | 1.15ms | — |
| proxy↔backend 往返 | ~4.1ms | ~3.9ms | 固定(~4ms) |
| 主要瓶颈 | NVMe 落盘(74%) + 往返 4.1ms | RNIC 天花板(10G) + 往返 3.9ms | — |

**一句话**: RDMA 数据面搬运能力极强(mock 10.3G = RNIC 天花板),真写被 NVMe 落盘拉到 3.56G(与 GDS 真写 3.3G 接近 —— NVMe 写带宽是两者共同天花板)。proxy↔backend 往返 ~4ms 是 RDMA 路径固定开销(比 GDS 的 ~1.5ms 大,但并发下被掩盖)。

> 注: 绝对值随盘态(SLC/TLC 混态)与 governor 变化;本次为 SLC 已耗后的 TLC 稳态值(预填充后)。RDMA 调参对 nt/cp/part 钝感(瓶颈在 RNIC 硬件),详见 `TUNING_DEEP_ANALYSIS.md`。
