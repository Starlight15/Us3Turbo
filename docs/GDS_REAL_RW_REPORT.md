# GDS 性能报告

**测试日期**: 2026-08-10

**环境**: governor=performance;client=backend 同机 192.168.1.198(mlx5_2,100Gb);NVMe `/dev/nvme1n1`;backend `[gds] worker_threads=4`;client concurrency=16。

**工具**: `us3_turbo_bench_gds_multipart`(写)、`us3_turbo_bench_gds_get`(读)。

**场景说明**

- **实际读写**: backend `[gds] mock_aio_write=0`、`mock_rdma_read=0` —— 真 RDMA READ 搬运 + NVMe 落盘;GET 真 RDMA WRITE 回 client。
- **mock 仅搬运**: backend `[gds] mock_aio_write=1` —— 跳过落盘,只测 client→proxy→backend RDMA READ 数据搬运。

---

## 1 实际读写

### 1.1 结论

分段上传 part=4M / nt=8 / cp=16,真写 ≈ 3.30 GiB/s,真读 ≈ 4.6 GiB/s(4M 对象,conc=16)。读 > 写(NVMe 读快、无写放大)。真写最优 part∈{2M,4M}(噪声内等效,选 4M:part 数减半、对齐 `max_single_put_bytes=4M`)——与 mock(8M)相反。

### 1.2 写性能扫描

#### 1.2.1 part_size

**测试条件**: 扫描 part_size ∈ {1M,2M,4M,8M,16M};固定 num_threads=16、conn_pool=16、concurrency=16;total=64M、reps=3、warmup=1。

| part_size | 吞吐 (MiB/s) | data-plane avg (ms) |
|---|---|---|
| 1M | 2893 | 340 |
| 2M | 3399 | 293 |
| 4M | 3675 | 272 |
| 8M | 2169 | 460 |
| 16M | 1613 | 612 |

小 part 并行度高、≥8M 输给 NVMe 写延迟/排队。2M 与 4M 噪声内等效。真写瓶颈是 NVMe,小 part 更优。**选 part_size=4M**:与 2M 同效但 part 数减半、对齐 `max_single_put_bytes=4M`。

#### 1.2.2 num_threads

**测试条件**: 扫描 num_threads ∈ {4,8,16};固定 part_size=4M、conn_pool=16、concurrency=16;total=64M、reps=3、warmup=1。

| num_threads | 吞吐 (MiB/s) |
|---|---|
| 4 | 3465 |
| 8 | 3512 |
| 16 | 3049 |

num_threads 4–16 扁平(单轮波动 ±7%)。真写瓶颈在 NVMe 不在 proxy 调度。**选 num_threads=8**(省线程,够用有余量)。

#### 1.2.3 backend_conn_pool_size

**测试条件**: 扫描 conn_pool ∈ {4,8,16};固定 part_size=4M、num_threads=8、concurrency=16;total=64M、reps=3、warmup=1。

| conn_pool | 吞吐 (MiB/s) |
|---|---|
| 4 | 3098 |
| 8 | 3114 |
| 16 | 3359 |

conn_pool 4–16 略升,cp=16 最高。**选 conn_pool=16**(给足连接余量)。

#### 1.2.4 综合最优

**part_size=4M / num_threads=8 / conn_pool=16 → ≈ 3.30 GiB/s**(8 轮均值,见 §1.5 条件)。

### 1.3 读性能

**测试条件**: 扫描 concurrency ∈ {4,8,16};固定 part_size=4M(对象大小)、num_threads=8、conn_pool=16;count=128、warmup=1。

| concurrency | 吞吐 (MiB/s) | ops/s | p95 (ms) |
|---|---|---|---|
| 4 | 4055 | 1014 | 5.8 |
| 8 | 4356 | 1089 | 10.1 |
| 16 | 4577 | 1144 | 21.2 |

conc 4–16 单调升,conc=16 近峰;**conc≥16 p95 升至 21ms**。读带宽 > 写带宽(NVMe 读无写放大,RDMA WRITE 一次推完)。

### 1.4 推荐配置

| 项 | 值 |
|---|---|
| `--multipart_part_size` | 4M |
| `--num_threads` | 8 |
| `--backend_conn_pool_size` | 16 |
| `[gds] worker_threads` | 4 |
| `--concurrency`(client) | 16 |
| 稳态吞吐 | 真写 ~3.30G / 真读 ~4.6G |

### 1.5 瓶颈分析

**测试条件**: part_size=4M、num_threads=8、conn_pool=16、concurrency=32(reps=3 warmup=1,8 轮)。单 part-put 端到端 ~12.3 ms。

**proxy 视角**

| 段 | avg (µs) | 占比 |
|---|---|---|
| validate | 325 | 3% |
| 等 backend(recv) | 10981 | 89% |
| WritePartIndex | 989 | 8% |

**backend 内**(total_recv_to_response ~8.42ms)

| 段 | avg (µs) | p95 (µs) | 说明 |
|---|---|---|---|
| main_to_done | 3471 | 7956 | 含 aio 完成等待 |
| queue_wait | 2728 | 9190 | 4 线程池排队 |
| aio_write | 2303 | 4427 | NVMe 落盘 |
| rdma_read | 923 | 1271 | RDMA READ 4M |
| memcpy | 341 | 369 | |
| crc32c | 235 | 275 | |

proxy recv 10981 − backend 8424 ≈ **2560µs = proxy↔backend 往返+框架开销**。

**瓶颈**: NVMe 落盘(aio_write 2.3ms + main_to_done 3.5ms 等完成)+ worker 4 线程池排队(queue_wait p95 9.2ms)。数据搬运(rdma_read 0.92ms)非瓶颈。真写受 NVMe 写延迟 + worker 串行化双重约束。

---

## 2 mock 仅搬运(跳过落盘)

### 2.1 吞吐

**测试条件**: part_size=4M、num_threads=8、conn_pool=16、concurrency=16、reps=3、warmup=1,8 轮。

| 轮次 | 吞吐 (MiB/s) |
|---|---|
| 1–8 | 3641 / 3707 / 3701 / 3720 / 3707 / 3682 / 3677 / 3557 |
| **均值** | **~3674**(区间 3557–3720,±2%) |

比实际写(3318)+11% —— NVMe 落盘约占真写端到端的 11%。

### 2.2 瓶颈分析

**测试条件**: 同 §2.1。单 part-put 端到端 ~8.3 ms。

**proxy 视角**

| 段 | avg (µs) | 占比 |
|---|---|---|
| validate | 338 | 4% |
| 等 backend(recv) | 6965 | 84% |
| WritePartIndex | 995 | 12% |

**backend 内**(total_recv_to_response ~4.84ms)

| 段 | avg (µs) | p95 (µs) | 说明 |
|---|---|---|---|
| queue_wait | 1107 | 3929 | 4 线程池排队 |
| rdma_read | 930 | 1690 | 真 RDMA READ 4M |
| done_to_response | 777 | 2256 | 完成到响应发出 |
| main_to_done | 620 | 785 | 落盘跳过(真写 3471) |
| memcpy | 428 | 537 | |
| crc32c | 253 | 358 | |

proxy recv 6965 − backend 4844 ≈ **2120µs = proxy↔backend 往返+框架开销**。

**瓶颈**: 排除落盘后,瓶颈转移到 ① worker 4 线程池排队(queue_wait 1.1ms,p95 3.9ms) ② proxy↔backend 往返+框架 ~2.1ms ③ done_to_response 0.78ms。均属调度/串行化开销,非数据搬运(rdma_read 0.93ms 健康)。mock 下固定开销摊薄,part 越大越优(8M 为峰),与真写相反。

---

## 3 两场景对比

| 维度 | 实际读写 | mock 仅搬运 | 差 |
|---|---|---|---|
| 写吞吐 | ~3.30G | ~3.67G | +11%(落盘) |
| backend total | 8.42ms | 4.84ms | -3.6ms(aio_write) |
| 主要瓶颈 | NVMe 落盘 + worker 排队 | 往返框架开销 + worker 长尾 | — |
| 最优 part | {2M,4M} | 8M | 相反 |

**一句话**: 数据搬运(RDMA READ)本身很快(~0.93ms),两端性能都耗在搬运之外——实际读写卡在 NVMe 落盘(占 11%),仅搬运卡在调度/框架往返与 worker 池长尾。

> 注: 绝对值随盘态(SLC/TLC 混态)与 governor 变化;本次为 SLC 已耗后的 TLC 稳态值(预填充后)。所有扫描/concurrency 均 ≤16。
