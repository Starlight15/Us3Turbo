# GDS 性能报告

**测试日期**: 2026-08-10

**环境**: governor=performance;client=backend 同机 192.168.1.198(mlx5_2,100Gb);NVMe `/dev/nvme1n1`;backend `[gds] worker_threads=4`;client 32 线程。

**工具**: `us3_turbo_bench_gds_multipart`(写)、`us3_turbo_bench_gds_get`(读)。

**场景说明**

- **实际读写**: backend `[gds] mock_aio_write=0`、`mock_rdma_read=0` —— 真 RDMA READ 搬运 + NVMe 落盘;GET 真 RDMA WRITE 回 client。
- **mock 仅搬运**: backend `[gds] mock_aio_write=1` —— 跳过落盘,只测 client→proxy→backend RDMA READ 数据搬运。

---

## 1 实际读写

### 1.1 结论

分段上传 part=4M / nt=8 / cp=32,真写 ≈ 3.30 GiB/s,真读 ≈ 5.3 GiB/s(4M 对象,conc=16)。读 > 写(NVMe 读快、无写放大)。真写最优 part∈{2M,4M}(噪声内等效,选 4M:part 数减半、对齐 `max_single_put_bytes=4M`)——与 mock(8M)相反。

### 1.2 写性能扫描

#### 1.2.1 part_size

**测试条件**: 扫描 part_size ∈ {1M,2M,4M,8M,16M};固定 num_threads=16、conn_pool=16;total=64M、concurrency=32、reps=3、warmup=1。

| part_size | 吞吐 (MiB/s) | data-plane avg (ms) |
|---|---|---|
| 1M | 3030 | 647 |
| 2M | 3635 | 537 |
| 4M | 3398 | 558 |
| 8M | 2373 | 775 |
| 16M | 1629 | 1030 |

小 part 并行度高、≥8M 输给 NVMe 写延迟/排队。2M 与 4M 相差 ~6%(单轮噪声内,实测等效)。真写瓶颈是 NVMe,小 part 更优。**选 part_size=4M**:与 2M 同效但 part 数减半、对齐 `max_single_put_bytes=4M`。

#### 1.2.2 num_threads

**测试条件**: 扫描 num_threads ∈ {4,8,16};固定 part_size=4M、conn_pool=32;total=64M、concurrency=32、reps=3、warmup=1。

| num_threads | 吞吐 (MiB/s) |
|---|---|
| 4 | 3449 |
| 8 | 3478 |
| 16 | 3604 |

num_threads 4–16 扁平(±2%,nt=16 略高)。真写瓶颈在 NVMe 不在 proxy 调度。**选 num_threads=8**(省线程,差异 <4%)。

#### 1.2.3 backend_conn_pool_size

**测试条件**: 扫描 conn_pool ∈ {4,8,16,32,48};固定 part_size=4M、num_threads=8;total=64M、concurrency=32、reps=3、warmup=1。

| conn_pool | 吞吐 (MiB/s) |
|---|---|
| 4 | 3213 |
| 8 | 3374 |
| 16 | 3308 |
| 32 | 3257 |
| 48 | 3303 |

conn_pool 4–48 扁平(3213–3374,±2.5%):cp=4 略低、cp≥8 均在 ~3300。4M 下 conn 占用对吞吐影响小于 2M(2M 时 cp 单调升、32–48 饱和、64 过配反降)。**选 conn_pool=32**(给足连接余量,差异 <3%)。

#### 1.2.4 综合最优

**part_size=4M / num_threads=8 / conn_pool=32 → ≈ 3.30 GiB/s**。

### 1.3 读性能

**测试条件**: 扫描 concurrency ∈ {4,8,16};固定 part_size=4M(对象大小)、num_threads=8、conn_pool=32;count=128、warmup=1。

| concurrency | 吞吐 (MiB/s) | ops/s | p95 (ms) |
|---|---|---|---|
| 4 | 4376 | 1094 | 6.5 |
| 8 | 5143 | 1286 | 9.2 |
| 16 | 5287 | 1322 | 17.8 |

conc 4–16 单调升(4.4→5.3 GiB/s),conc=16 近峰;**conc≥16 p95 升至 18ms**。读带宽 > 写带宽(NVMe 读无写放大,RDMA WRITE 一次推完)。

### 1.4 推荐配置

| 项 | 值 |
|---|---|
| `--multipart_part_size` | 4M |
| `--num_threads` | 8 |
| `--backend_conn_pool_size` | 32 |
| `[gds] worker_threads` | 4 |
| 稳态吞吐 | 真写 ~3.30G / 真读 ~5.3G |

### 1.5 瓶颈分析

**测试条件**: part_size=4M、num_threads=8、conn_pool=32、concurrency=32。单 part-put 端到端 ~8.7 ms。

**proxy 视角**

| 段 | avg (µs) | 占比 |
|---|---|---|
| validate | 341 | 4% |
| 等 backend(recv) | 7347 | 84% |
| WritePartIndex | 1034 | 12% |

连接池无争抢(1µs)、TCP 发 8µs;recv 的 7.3ms 纯粹等 backend。

**backend 内**(total_recv_to_response ~7.35ms)

| 段 | avg (µs) | p95 (µs) | 说明 |
|---|---|---|---|
| worker queue_wait | 2432 | 5218 | 4 线程池排队 |
| rdma_read | 963 | 1323 | RDMA READ 4M |
| aio_write | 2159 | 3915 | NVMe 落盘 |
| main_to_done | 2904 | 4679 | 含 aio 完成等待 |
| 其他(memcpy/crc) | ~700 | — | |

proxy recv 7347 − backend 7351 ≈ 0(backend 处理占满,无框架往返 gap)。

**瓶颈**: NVMe 落盘(aio_write 2.16ms + main_to_done 2.9ms 等完成)+ worker 4 线程池排队(p95 5.2ms)。数据搬运(rdma_read 0.96ms)非瓶颈;proxy 仅占 ~16%。真写受 NVMe 写延迟 + worker 串行化双重约束,故 worker_threads=4 为峰、8 过提交反降。

---

## 2 mock 仅搬运(跳过落盘)

### 2.1 吞吐

**测试条件**: part_size=4M、num_threads=8、conn_pool=32、concurrency=32、reps=3、warmup=1,8 轮。

3947 / 3649 / 3727 / 4121 / 4134 / 4187 / 3974 / 3789 MiB/s → **均值 ~3941 MiB/s**(区间 3649–4187,±7%)。比实际读写(3298)+20% —— 即 NVMe 落盘约占端到端 20%。

### 2.2 瓶颈分析

**测试条件**: part_size=4M、num_threads=8、conn_pool=32、concurrency=32。单 part-put 端到端 ~6.9 ms。

**proxy 视角**

| 段 | avg (µs) | 占比 |
|---|---|---|
| validate | 341 | 5% |
| 等 backend(recv) | 5556 | 80% |
| WritePartIndex | 1031 | 15% |

**backend 内**(total_recv_to_response ~4.06ms)

| 段 | avg (µs) | p95 (µs) | 说明 |
|---|---|---|---|
| worker queue_wait | 988 | 3202 | 4 线程池排队长尾(max 15.5ms) |
| rdma_read | 940 | 1662 | 真 RDMA READ 4M,单流 ~4.4G |
| done_to_response | 510 | 1454 | 完成到响应发出 |
| 其他(main/memcpy/crc) | ~1630 | — | |

proxy recv 5556 − backend 4064 ≈ **1500µs = proxy↔backend 往返+框架开销**。

**瓶颈**: 排除落盘后,瓶颈转移到 ① proxy↔backend 往返+框架开销 ~1.5ms/part ② backend 4 线程 worker 池队列等待长尾(p95 3.2ms)。均属调度/串行化开销,非数据搬运(RDMA READ 0.94ms 健康)。mock 下固定开销摊薄,故 part 越大越优(8M 为峰),与真写相反。

---

## 3 两场景对比

| 维度 | 实际读写 | mock 仅搬运 | 差 |
|---|---|---|---|
| 写吞吐 | ~3.30G | ~3.94G | +20%(落盘) |
| backend total | 7.35ms | 4.06ms | +3.3ms(aio_write) |
| 主要瓶颈 | NVMe 落盘 + worker 排队 | 往返框架开销 + worker 长尾 | — |
| 最优 part | {2M,4M} | 8M | 相反 |

**一句话**: 数据搬运(RDMA READ)本身很快(~0.94ms),两端性能都耗在搬运之外——实际读写卡在 NVMe 落盘(占 20%),仅搬运卡在调度/框架往返与 worker 池长尾。

> 注: 绝对值随盘态(SLC/TLC 混态)与 governor 变化;本次为 SLC 已耗后的 TLC 稳态值(预填充后)。
