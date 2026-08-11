# RDMA 性能报告

**测试日期**: 2026-08-10(本日多轮复测至稳态:重启 backend 后须充分预热 NVMe + RDMA-CM,否则冷态写吞吐跌至 ~1.7G;预热数轮后回到稳态值见下)

**环境**: governor=performance;client=backend 同机 192.168.1.198(mlx5_2,100Gb);NVMe `/dev/nvme1n1`(裸盘,TLC 稳态,smart-log Data Units Written 82.06 TB);backend `[rdma] worker_threads=4`;client concurrency=16。CPU=Xeon 8358P(128 核)/2.0TiB MEM。RDMA 数据面=host 内存(libibverbs RC,经 RDMA CM)。

**工具**: `us3_turbo_bench_rdma_multipart`(写)、`us3_turbo_bench_rdma_get`(读)。

**场景说明**

- **实际读写**: backend `[rdma] mock_aio_write=0`、`mock_mode=0` —— 真 RDMA READ 搬运 + NVMe 落盘;GET 真 RDMA WRITE 回 client。
- **mock 仅搬运**: backend `[rdma] mock_aio_write=1` —— 跳过落盘,只测 client→proxy→backend RDMA READ 数据搬运。

---

## 1 实际读写

### 1.1 结论

分段上传 part=4M / nt=8 / cp=16,真写 ≈ 3.7 GiB/s(预热后稳态 3.66–3.81G;冷态 ~1.7G,须充分预热),真读 ≈ 4.7 GiB/s(4M 对象,conc=16,4 轮均值 conc4=4050/conc8=4557/conc16=4480,稳态 conc16=4766)。读 > 写(NVMe 读快于写、无写放大,与 GDS 同)。真写被 NVMe 落盘拉低(NVMe 写 util 达 99%),搬运能力被掩盖(见 §2 mock)。

### 1.2 写性能

**测试条件**: part_size=4M、num_threads=8、conn_pool=16、concurrency=16、reps=3、warmup=1,8 轮(预热后稳态)。

| 轮次 | 吞吐 (MiB/s) |
|---|---|
| 1–8 | 3806 / 3696 / 3799 / 3720 / 3662 / 3773 / 3783 / 3695 |
| **均值** | **~3742**(区间 3662–3806,±4%;稳态 reps=20=3813) |

#### 1.2.1 num_threads 扫描

**测试条件**: 扫描 num_threads ∈ {4,8,16};固定 part_size=4M、conn_pool=16、concurrency=16、reps=3、warmup=1。

| num_threads | 吞吐 (MiB/s) |
|---|---|
| 4 | 3537 |
| 8 | 3049 |
| 16 | 3052 |

num_threads 4–16 钝感(单轮波动,瓶颈在 NVMe 落盘,与 GDS 同;mock 下瓶颈在 host 搬运 dispatch,同机不经 RNIC 物理链路)。**选 num_threads=8**(省线程)。

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
| 稳态吞吐 | 真写 ~3.7G / 真读 ~4.7G |

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

**瓶颈**: NVMe 落盘(disk_wait 3.4ms,占 backend 70%)+ proxy↔backend 往返+框架 ~5.2ms。数据搬运(rdma_read 0.88ms)非瓶颈。真写下搬运能力被 NVMe 写带宽掩盖。复测确认:稳态 NVMe 写 util 达 99%(高于 GDS 84%,因 RDMA 提交 AIO 队列更深),吞吐 ~3.7G。

### 1.6 系统资源占用(稳态,part=4M/nt=8/cp=16/conc=16)

**写**(reps=20 稳态 ~3.8G):

| 资源 | 用量 | 说明 |
|---|---|---|
| backend CPU | ~92%(≈0.9 核/128) | 真写 NVMe-bound,CPU 富余 |
| proxy CPU | ~17% | |
| backend RSS | ~1119 MiB | MR pool + 缓冲 |
| proxy RSS | ~22 MiB | |
| NVMe nvme1n1 | ~3.9 GB/s 写,~7900 w/s,**99% util** | 真写瓶颈(NVMe 近饱和) |
| RNIC phy(ens13f0np0) | ≈0 | 同机 loopback,不经物理链路(见 §3) |
| GPU | N/A | RDMA 数据面=host 内存,不占 GPU |

**读**(conc=16,count=512 稳态 ~4.8G):

| 资源 | 用量 | 说明 |
|---|---|---|
| backend CPU | ~45% | |
| NVMe nvme1n1 | ~1.5 GB/s 读,**46% util** | 读非 NVMe 饱和,瓶颈在 worker 串行 + 往返 |
| RNIC phy | ≈0 | 同机 loopback |

---

## 2 mock 仅搬运(跳过落盘)

### 2.1 吞吐

**测试条件**: part_size=4M、num_threads=8、conn_pool=16、concurrency=16、reps=3、warmup=1,8 轮。

| 轮次 | 吞吐 (MiB/s) |
|---|---|
| 1–8 | 9825 / 9834 / 9290 / 9942 / 9577 / 9906 / 9886 / 10104 |
| **均值** | **~9795**(区间 9290–10104,±4%;稳态 reps=20=10356) |

≈ 9.8–10.4 GiB/s。**注意:此值非 RNIC 100Gb 物理带宽天花板**——实测 RNIC 物理口 `rx/tx_bytes_phy` 在 10G 搬运期间增量 ≈0(同机部署走网卡内部 loopback/host 路径,不经物理链路);~10G 上限来自 host 侧搬运(RDMA READ dispatch + CPU 调度,backend CPU 仅 ~69% 仍有裕量,瓶颈在 dispatch 串行 + 往返)。比实际写(3742)+162% —— NVMe 落盘占真写端到端的 ~62%。

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

**瓶颈**: 并发下吞吐 ~10G(实测 RNIC 物理口 phy 计数 ≈0 —— 同机走网卡内部 loopback,不经物理链路;故此 10G 非 RNIC 100Gb 带宽上限,而是 host 侧搬运 dispatch 串行 + 往返的天花板,backend CPU ~69% 仍有裕量)。单 part 延迟 3.7ms,搬运 1.5ms + 往返 0.7ms。提并发可进一步逼近 ~10.4G(host 搬运上限)。mock 下固定开销摊薄,part 越大越优(8M 为峰),与真写相反。

### 2.3 系统资源占用(稳态 mock,part=4M/nt=8/cp=16/conc=16)

| 资源 | mock 写 | 对比真写 |
|---|---|---|
| backend CPU | ~69% | 低于真写 92%(瓶颈不在 CPU,在 dispatch/loopback) |
| proxy CPU | ~33% | 高于真写 17%(搬运更快,proxy 更忙) |
| backend RSS | ~1003 MiB | 相近 |
| NVMe nvme1n1 | idle(~1.4 MB/s binlog,util 0.05%) | 真写 3.9 GB/s/99% — 确认 mock 跳过落盘 |
| RNIC phy | ≈0 | 同机 loopback,**不经物理链路**(关键修正) |
| GPU | N/A | host 内存面 |

---

## 3 两场景对比

| 维度 | 实际读写 | mock 仅搬运 | 差 |
|---|---|---|---|
| 写吞吐 | ~3.7G | ~9.8G | +162%(落盘) |
| 真读吞吐 | ~4.7G | —(无落盘数据) | — |
| backend CPU | ~92%(0.9 核) | ~69% | mock 更闲(瓶颈不在 CPU) |
| backend RSS | ~1119 MiB | ~1003 MiB | 相近 |
| NVMe util | 99%(3.9G 写) | 0.05%(跳过) | 落盘 = 真写瓶颈 |
| backend total | 4.85ms | 1.50ms | -3.35ms(disk_wait) |
| disk_wait | 3404µs | 15µs | 落盘 |
| rdma_read | 0.88ms | 1.08ms | — |
| proxy↔backend 往返 | ~5.2ms | ~0.7ms | 真写大(mock backend 慢→proxy 等更久,含 backend 内等待) |
| 主要瓶颈 | NVMe 落盘(70%,util 99%) | host 搬运 dispatch 串行 | — |
| RNIC phy | ≈0 | ≈0 | 同机 loopback,不经物理链路 |

 RDMA 数据面搬运能力极强(mock ~9.8G),真写被 NVMe 落盘拉到 ~3.7G(与 GDS 真写 3.3G 接近 —— NVMe 写带宽是两者共同天花板;RDMA 提交 AIO 队列更深故 NVMe util 99% 高于 GDS 84%,吞吐也更高)。**关键修正:同机部署下 RNIC 物理口 phy 计数全程 ≈0 —— 数据搬运走网卡内部 loopback,不经 100Gb 物理链路;故 mock ~10G 非 RNIC 带宽上限,而是 host 侧搬运上限。跨机部署才会经 RNIC 物理链路、受 100Gb 带宽约束。** proxy↔backend 往返在真写下 ~5.2ms(含 backend 内等待放大),mock 下 backend 快则往返降至 0.7ms。

> 注: 绝对值随盘态(SLC/TLC 混态)与 governor 变化;本次为 SLC 已耗后的 TLC 稳态值(预填充后)。所有扫描/concurrency 均 ≤16。**重启 backend 后须充分预热 NVMe + RDMA-CM**(冷态写吞吐跌至 ~1.7G,预热数轮后回到 ~3.7G 稳态)。RDMA 调参对 nt/cp/part 钝感(瓶颈在 NVMe/host 搬运),详见 `TUNING_DEEP_ANALYSIS.md`。
