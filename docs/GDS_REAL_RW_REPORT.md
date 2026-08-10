# GDS 真实读写性能报告

**测试日期**:2026-08-10  **场景**:真写(`[gds] mock_aio_write=0`、`mock_rdma_read=0`)+ 真读(GDS GET,后端 `ReadDataDirectExternal`+RDMA WRITE 回 client)
**环境**:governor=performance;client=backend 同机 192.168.1.198(mlx5_2,100Gb);NVMe `/dev/nvme1n1`;backend `[gds] worker_threads=4`(固定);client 32 线程
**工具**:`us3_turbo_bench_gds_multipart`(写)、新增 `us3_turbo_bench_gds_get`(读)

---

## 1. 结论(一句话)

**真写峰值 ≈ 3.74 GiB/s(part=2M/nt=8/cp=32),真读峰值 ≈ 5.32 GiB/s(4M 对象/conc=32)。读 > 写(NVMe 读快、无写放大)。真写最优 part=2M——与 mock(8M)相反。**

---

## 2. 写:三轴扫描(真写)

### 2.1 part_size(nt=16/cp=16 固定,真写)

| part | 吞吐 (MiB/s) | data-plane avg (ms) |
|---|---|---|
| 1M | 3030 | 647 |
| **2M** | **3635** ★ | 537 |
| 4M | 3398 | 558 |
| 8M | 2373 | 775 |
| 16M | 1629 | 1030 |

**峰在 2M**,随 part 单调下降(1M 输给固定开销,≥4M 输给 NVMe 写延迟/排队)。mock 下 8M 为峰,因 mock 跳盘写、瓶颈是固定开销摊薄;**真写瓶颈是 NVMe,小 part 并行度更高 → 2M 最优**。

### 2.2 num_threads(part=2M,cp=32 固定,真写)

| nt | 吞吐 (MiB/s) |
|---|---|
| 4 | 3666 |
| 8 | 3619 |
| 16 | 3612 |
| 32 | 3272 |

nt 4–16 **扁平**(±1.5%),nt=32 反降 -10%。真写瓶颈在 NVMe 不在 proxy 调度,少线程即够;nt=32 增争抢。**选 nt=8**(够用有余量,省线程)。

### 2.3 backend_conn_pool_size(part=2M,nt=8 固定,真写)

| cp | 吞吐 (MiB/s) |
|---|---|
| 4 | 3497 |
| 8 | 3601 |
| 16 | 3684 |
| 32 | 3736 |
| 48 | 3740 ★ |
| 64 | 3255 |

cp≥nt 单调升,32–48 饱和,64 过配反降 -13%。**选 cp=32**(近峰 3736,比 48 省连接)。

### 2.4 写综合最优

**part=2M / nt=8 / cp=32 → 3736 MiB/s**(cp=48 仅 +0.1%,不值)。

---

## 3. 读:GDS GET(真读)

4M 对象(GET 受 client `put_single_max_bytes=4M` 限,单 PUT 播种),nt=8/cp=32,count=128,warmup=1:

| conc | 吞吐 (MiB/s) | ops/s | p50 (ms) | p95 (ms) |
|---|---|---|---|---|
| 16 | 4701 | 1175 | 12.5 | 20.2 |
| **32** | **5318** ★ | 1329 | 21.8 | 38.2 |
| 48 | 4549 | 1137 | 34.9 | 65.6 |
| 64 | 4775 | 1194 | 40.4 | 80.8 |

**真读峰值 ≈ 5318 MiB/s @ conc=32**。conc>32 因 client/proxy 端争抢 p95 飙到 65–81ms 而回落。读带宽 > 写带宽(NVMe 读无 SLC/TLC 写放大,RDMA WRITE 一次推完)。

---

## 4. 原因(精炼)

- **真写 part 峰在 2M 而非 8M**:真写瓶颈=NVMe。小 part → 更多并行 AIO 更好地喂 NVMe 队列;≥4M 单 AIO 写延迟/tail 上升 + GDS worker(wt=4)串行更久 → 聚合反降。mock 跳盘写,瓶颈是固定开销摊薄 → 8M 才峰。**两 regime 甜点不同,mock 调参不可直接搬到真写。**
- **nt 钝感**:真写瓶颈在 NVMe/backend,proxy 调度非瓶颈,nt 4–16 扁平;nt=32 只增 conn 争抢。
- **cp≥nt 单调升**:真写下 conn 占用久(part 落盘),cp<nt 时争抢明显;cp=32–48 喂饱,cp=64 过配反降。
- **读 > 写**:NVMe 读快于写(无写放大),GET 路径 RDMA WRITE 一次推对象;conc=32 后 client 端成瓶颈。

---

## 5. 推荐配置(真写)与历史对比

| 项 | mock 时代(旧) | **真写(本次推荐)** |
|---|---|---|
| `--multipart_part_size` | 8M | **2M** |
| `--num_threads` | 16 | **8** |
| `--backend_conn_pool_size` | 16 | **32** |
| `[gds] worker_threads` | 4 | 4(不变) |
| 峰值吞吐 | mock~4200(不可比) | **真写 3736 / 真读 5318** |

> 注:绝对值随盘态(SLC/TLC 混态)与 governor 变化;本次为 SLC 已耗后的 TLC 稳态值(预填充后),具可比性。

---

## 6. 复现

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
# 改 proxy flags(--num_threads/--backend_conn_pool_size/--multipart_part_size)后重启 proxy
# 写:真写 multipart,part 须与 --multipart_part_size 一致
./build/rtest/bench/gds/us3_turbo_bench_gds_multipart --part-size 2M --total 64M --concurrency 32 --reps 3 --warmup 1
# 读:先 PUT 播种再并发 GET(4M 受 client put_single_max_bytes 限)
./build/rtest/bench/gds/us3_turbo_bench_gds_get --size 4M --count 128 --concurrency 32 --warmup 1
```

---

## 7. 复测验证（2026-08-10 多轮）

多轮复测校验上文峰值。环境同 §0（governor=performance、同机、`[gds] mock 全关` 真写真读）。

**写峰值（part=2M/nt=8/cp=32，8 轮）**：3154 / 3214 / 3198 / 3398 / 3152 / 3233 / 3291 / 3232 MiB/s → 均值 ~3239（区间 3152–3398，±4%）。§2.4 报 3736 → **偏低约 13%**，但稳定、单调性结论不变。

**读峰值（4M/conc=32，6 轮）**：4310 / 4372 / 4553 / 4597 / 4014 / 4212 MiB/s → 均值 ~4343（区间 4014–4597，±7%）。§3 报 5318 → **偏低约 18%，且 5318 峰值 6 轮内不可复现**（实测最高 4597）。读波动显著大于写，conc=32 单次易受 client MR re-register 抖动影响。

**真实路径核查**：① bench 调 `UploadPartGds`/`GetObjectGds` 走 GDS 数据面，非 RDMA/UCX 旁路；② backend 启动日志 `mock_rdma_read:0, mock_aio_write:0`；③ `--verify-crc32c` 64/64 `crc32c MATCH`；④ 裸盘 `nvme smart-log` 写入量增量 525 单位(262.5MiB) ≈ 写入数据 256MiB（写放大 1.025）。四层证据确认真写真读，非 mock。

**结论**：原文档趋势性结论（真写峰在 part=2M、读>写、conc↑→p95↑）经多轮复测成立；绝对峰值偏乐观约 13–18%，建议引用时按写 ~3.2G / 读 ~4.3G 取值或标注"单次最优"。盘累计已写 81.25 TB，处纯 TLC 稳态，与原文前提一致，故偏低非盘态从空变满所致。
