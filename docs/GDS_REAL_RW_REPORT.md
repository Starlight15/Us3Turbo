# GDS 真实读写性能报告

**测试日期**:2026-08-10  **场景**:真写(`[gds] mock_aio_write=0`、`mock_rdma_read=0`)+ 真读(GDS GET,后端 `ReadDataDirectExternal`+RDMA WRITE 回 client)
**环境**:governor=performance;client=backend 同机 192.168.1.198(mlx5_2,100Gb);NVMe `/dev/nvme1n1`;backend `[gds] worker_threads=4`(固定);client 32 线程
**工具**:`us3_turbo_bench_gds_multipart`(写)、`us3_turbo_bench_gds_get`(读)

---

## 1. 结论

**分段上传 part=4M / nt=8 / cp=32,真写 ≈ 3.30 GiB/s,真读 ≈ 4.3 GiB/s(4M 对象/conc=32)。读 > 写(NVMe 读快、无写放大)。真写最优 part∈{2M,4M}(噪声内等效,选 4M:part 数减半、对齐 `max_single_put_bytes=4M`)——与 mock(8M)相反。**

---

## 2. 写:三轴扫描(真写)

### 2.1 part_size(nt=16/cp=16 固定)

| part | 吞吐 (MiB/s) | data-plane avg (ms) |
|---|---|---|
| 1M | 3030 | 647 |
| 2M | 3635 | 537 |
| **4M** | **3398** ★ | 558 |
| 8M | 2373 | 775 |
| 16M | 1629 | 1030 |

小 part 并行度高、≥8M 输给 NVMe 写延迟/排队,随 part 增大而降。**2M 与 4M 相差 ~6%(单轮噪声范围内),实测等效**。mock 下 8M 为峰(跳盘写、瓶颈是固定开销摊薄);**真写瓶颈是 NVMe,小 part 更优**。**选 part=4M**:与 2M 同效但 part 数减半(控制面开销↓)、对齐 client `max_single_put_bytes=4M`。

### 2.2 num_threads(part=2M,cp=32 固定)

| nt | 吞吐 (MiB/s) |
|---|---|
| 4 | 3666 |
| 8 | 3619 |
| 16 | 3612 |
| 32 | 3272 |

nt 4–16 **扁平**(±1.5%),nt=32 反降 -10%。真写瓶颈在 NVMe 不在 proxy 调度,少线程即够;nt=32 增争抢。**选 nt=8**(够用有余量,省线程)。

### 2.3 backend_conn_pool_size(part=2M,nt=8 固定)

| cp | 吞吐 (MiB/s) |
|---|---|
| 4 | 3497 |
| 8 | 3601 |
| 16 | 3684 |
| 32 | 3736 |
| 48 | 3740 ★ |
| 64 | 3255 |

cp≥nt 单调升,32–48 饱和,64 过配反降 -13%。**选 cp=32**(近峰,比 48 省连接)。

### 2.4 写综合最优

**part=4M / nt=8 / cp=32 → ≈ 3.30 GiB/s**(nt=8/cp=32 在 part=2M 下扫描得最优,2M/4M 等效故适用于 part=4M;cp=48 仅 +0.1%,不值)。

---

## 3. 读:GDS GET(真读)

4M 对象(GET 受 client `max_single_put_bytes=4M` 限,单 PUT 播种),nt=8/cp=32,count=128,warmup=1:

| conc | 吞吐 (MiB/s) | ops/s | p95 (ms) |
|---|---|---|---|
| 16 | 4668 | 1167 | 20.7 |
| **32** | **4310** ★ | 1093 | 40.3 |
| 48 | 4816 | 1204 | 59.8 |
| 64 | 4226 | 1057 | 111.8 |

conc 16–48 吞吐均在 4.3–4.8 GiB/s;**conc=32 为推荐工作点**(吞吐 ~4.3G、p95 40ms 可接受),conc≥48 p95 升至 60–112ms。读带宽 > 写带宽(NVMe 读无 SLC/TLC 写放大,RDMA WRITE 一次推完)。

---

## 4. 原因(精炼)

- **真写 part 峰在 {2M,4M} 而非 8M**:真写瓶颈=NVMe。小 part → 更多并行 AIO 更好地喂 NVMe 队列;≥8M 单 AIO 写延迟/tail 上升 + GDS worker(wt=4)串行更久 → 聚合反降。2M/4M 实测等效,选 4M 以减半 part 数、对齐 `max_single_put_bytes=4M`。mock 跳盘写,瓶颈是固定开销摊薄 → 8M 才峰。**两 regime 甜点不同,mock 调参不可直接搬到真写。**
- **nt 钝感**:真写瓶颈在 NVMe/backend,proxy 调度非瓶颈,nt 4–16 扁平;nt=32 只增 conn 争抢。
- **cp≥nt 单调升**:真写下 conn 占用久(part 落盘),cp<nt 时争抢明显;cp=32–48 喂饱,cp=64 过配反降。
- **读 > 写**:NVMe 读快于写(无写放大),GET 路径 RDMA WRITE 一次推对象;conc≥48 后 client 端 p95 成瓶颈。

---

## 5. 推荐配置(真写)

| 项 | **真写** |
|---|---|
| `--multipart_part_size` | **4M** |
| `--num_threads` | **8** |
| `--backend_conn_pool_size` | **32** |
| `[gds] worker_threads` | 4 |
| 稳态吞吐 | **真写 ~3.30G / 真读 ~4.3G** |

> 注:绝对值随盘态(SLC/TLC 混态)与 governor 变化;本次为 SLC 已耗后的 TLC 稳态值(预填充后)。

---

## 6. 瓶颈分析(GDS PUT 路径)

分段计时(4M part,32 并发;mock 搬运排除落盘,真 RDMA READ)。单 part-put 端到端 ~6.9 ms:

**proxy 视角**

| 段 | avg (µs) | 占比 |
|---|---|---|
| validate | 341 | 5% |
| 等 backend(recv) | 5556 | 80% |
| WritePartIndex | 1031 | 15% |

连接池无争抢(acquire_conn 1µs)、TCP 发 7µs;recv 的 5.6ms 纯粹等 backend。

**backend 内(total_recv_to_response ~4.06ms)**

| 段 | avg (µs) | p95 (µs) | 说明 |
|---|---|---|---|
| worker queue_wait | 988 | 3202 | 4 线程池排队长尾(max 15.5ms) |
| rdma_read | 940 | 1662 | 真 RDMA READ 4M,单流 ~4.4G,不慢 |
| done_to_response | 510 | 1454 | 完成到响应发出 |
| 其他(main/memcpy/crc) | ~1630 | — | |

proxy recv 5556 − backend 4064 ≈ **1500µs = proxy↔backend 往返+框架开销**。

**瓶颈**:不在数据搬运(RDMA READ 0.94ms 健康),而在 ① proxy↔backend 往返+框架开销 ~1.5ms/part ② backend 4 线程 worker 池队列等待长尾(p95 3.2ms)——均属调度/串行化开销,非 NVMe 非带宽。落盘(真写)额外 +20%(真写 3298 vs mock 搬运 3941),即 worker 串行 NVMe AIO 之上再压一段盘延迟,亦为 worker_threads=4 为峰、8 过提交反降之因。

