# GDS 性能报告

**测试日期**:2026-08-10  **环境**:governor=performance;client=backend 同机 192.168.1.198(mlx5_2,100Gb);NVMe `/dev/nvme1n1`;backend `[gds] worker_threads=4`;client 32 线程
**工具**:`us3_turbo_bench_gds_multipart`(写)、`us3_turbo_bench_gds_get`(读)

两个场景:
- **A. 实际读写**:backend `[gds] mock_aio_write=0`、`mock_rdma_read=0` —— 真 RDMA READ 搬运 + NVMe 落盘;GET 真 RDMA WRITE 回 client。
- **B. mock 仅搬运**:backend `[gds] mock_aio_write=1` —— 跳过落盘,只测 client→proxy→backend RDMA READ 数据搬运。

---

## A. 实际读写

### A.1 结论

**分段上传 part=4M / nt=8 / cp=32,真写 ≈ 3.30 GiB/s,真读 ≈ 4.3 GiB/s(4M 对象/conc=32)。读 > 写(NVMe 读快、无写放大)。真写最优 part∈{2M,4M}(噪声内等效,选 4M:part 数减半、对齐 `max_single_put_bytes=4M`)——与 mock(8M)相反。**

### A.2 写三轴扫描

**part_size(nt=16/cp=16 固定)**

| part | 吞吐 (MiB/s) | data-plane avg (ms) |
|---|---|---|
| 1M | 3030 | 647 |
| 2M | 3635 | 537 |
| **4M** | **3398** ★ | 558 |
| 8M | 2373 | 775 |
| 16M | 1629 | 1030 |

小 part 并行度高、≥8M 输给 NVMe 写延迟/排队。2M 与 4M 相差 ~6%(单轮噪声内,实测等效)。真写瓶颈是 NVMe,小 part 更优。**选 part=4M**:与 2M 同效但 part 数减半、对齐 `max_single_put_bytes=4M`。

**num_threads(part=2M,cp=32 固定)**:nt 4–16 扁平(±1.5%),nt=32 反降 -10%。真写瓶颈在 NVMe 不在 proxy 调度。**选 nt=8**。

**backend_conn_pool_size(part=2M,nt=8 固定)**:cp≥nt 单调升,32–48 饱和,64 过配反降 -13%。**选 cp=32**。

**综合最优**:**part=4M / nt=8 / cp=32 → ≈ 3.30 GiB/s**。

### A.3 读 GDS GET(真读)

4M 对象,nt=8/cp=32,count=128,warmup=1:

| conc | 吞吐 (MiB/s) | ops/s | p95 (ms) |
|---|---|---|---|
| 16 | 4668 | 1167 | 20.7 |
| **32** | **4310** ★ | 1093 | 40.3 |
| 48 | 4816 | 1204 | 59.8 |
| 64 | 4226 | 1057 | 111.8 |

conc 16–48 均在 4.3–4.8 GiB/s;**conc=32 为推荐工作点**,conc≥48 p95 升至 60–112ms。读带宽 > 写带宽(NVMe 读无写放大,RDMA WRITE 一次推完)。

### A.4 推荐配置(实际读写)

| 项 | 值 |
|---|---|
| `--multipart_part_size` | **4M** |
| `--num_threads` | **8** |
| `--backend_conn_pool_size` | **32** |
| `[gds] worker_threads` | 4 |
| 稳态吞吐 | **真写 ~3.30G / 真读 ~4.3G** |

### A.5 瓶颈分析(实际读写,4M part,32 并发)

单 part-put 端到端 ~8.7 ms:

**proxy 视角**

| 段 | avg (µs) | 占比 |
|---|---|---|
| validate | 341 | 4% |
| 等 backend(recv) | 7347 | 84% |
| WritePartIndex | 1034 | 12% |

连接池无争抢(1µs)、TCP 发 8µs;recv 的 7.3ms 纯粹等 backend。

**backend 内(total_recv_to_response ~7.35ms)**

| 段 | avg (µs) | p95 (µs) | 说明 |
|---|---|---|---|
| worker queue_wait | 2432 | 5218 | 4 线程池排队 |
| rdma_read | 963 | 1323 | RDMA READ 4M |
| **aio_write** | **2159** | 3915 | **NVMe 落盘** |
| main_to_done | 2904 | 4679 | 含 aio 完成等待 |
| 其他(memcpy/crc) | ~700 | — | |

proxy recv 7347 − backend 7351 ≈ 0(无框架往返 gap,backend 处理占满)。

**瓶颈**:**NVMe 落盘(aio_write 2.16ms + main_to_done 2.9ms 等完成)+ worker 4 线程池排队(p95 5.2ms)**。数据搬运(rdma_read 0.96ms)非瓶颈;proxy 仅占 ~16%。真写受 NVMe 写延迟 + worker 串行化双重约束,故 worker_threads=4 为峰、8 过提交反降。

---

## B. mock 仅搬运(跳过落盘)

### B.1 吞吐

part=4M / nt=8 / cp=32 / conc=32,8 轮:3947 / 3649 / 3727 / 4121 / 4134 / 4187 / 3974 / 3789 MiB/s → **均值 ~3941 MiB/s**(区间 3649–4187,±7%)。比实际读写(3298)+20% —— 即 NVMe 落盘约占端到端 20%。

### B.2 瓶颈分析(仅搬运,4M part,32 并发)

单 part-put 端到端 ~6.9 ms:

**proxy 视角**

| 段 | avg (µs) | 占比 |
|---|---|---|
| validate | 341 | 5% |
| 等 backend(recv) | 5556 | 80% |
| WritePartIndex | 1031 | 15% |

**backend 内(total_recv_to_response ~4.06ms)**

| 段 | avg (µs) | p95 (µs) | 说明 |
|---|---|---|---|
| worker queue_wait | 988 | 3202 | 4 线程池排队长尾(max 15.5ms) |
| rdma_read | 940 | 1662 | 真 RDMA READ 4M,单流 ~4.4G |
| done_to_response | 510 | 1454 | 完成到响应发出 |
| 其他(main/memcpy/crc) | ~1630 | — | |

proxy recv 5556 − backend 4064 ≈ **1500µs = proxy↔backend 往返+框架开销**。

**瓶颈**:排除落盘后,瓶颈转移到 ① proxy↔backend 往返+框架开销 ~1.5ms/part ② backend 4 线程 worker 池队列等待长尾(p95 3.2ms)。均属调度/串行化开销,非数据搬运(RDMA READ 0.94ms 健康)。mock 下固定开销摊薄,故 part 越大越优(8M 为峰),与真写相反。

---

## C. 两场景对比

| | 实际读写 | mock 仅搬运 | 差 |
|---|---|---|---|
| 写吞吐 | ~3.30G | ~3.94G | +20%(落盘) |
| backend total | 7.35ms | 4.06ms | +3.3ms(aio_write) |
| 主要瓶颈 | NVMe 落盘 + worker 排队 | 往返框架开销 + worker 长尾 | |
| 最优 part | {2M,4M} | 8M | 相反 |

**一句话**:数据搬运(RDMA READ)本身很快(~0.94ms),两端性能都耗在搬运之外——实际读写卡在 NVMe 落盘(占 20%),仅搬运卡在调度/框架往返与 worker 池长尾。

> 注:绝对值随盘态(SLC/TLC 混态)与 governor 变化;本次为 SLC 已耗后的 TLC 稳态值(预填充后)。
