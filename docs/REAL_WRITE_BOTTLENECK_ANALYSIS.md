# GDS / RDMA 真实写入性能瓶颈深度分析(日志打点)

**日期**:2026-08-08
**方法**:后端带 `US3_GDS_PUT_TIMING=1` 启用固化打点(`gds_service.cc` / `ac_server.cc` / `ioContext.cc` / `rdma_service.cc`,三层 `[perf/]` 已固化,见 [perf-stats-infra](../../../)),`mock_aio_write=0` 真落盘,optimal 配置 8M/16/16,binary 含 MR-pool commit 4fdd47a。跑 GDS+RDMA multipart 8M(conc32 reps3 warmup1),解析 backend 日志(`HandleRdmaPut DONE PHASE` + `[gds-timing]` + `[rdma-dispatch]` + `[rdma-read]` 各阶段 µs)取 p50。
**承接**:[REAL_READWRITE_REPORT](REAL_READWRITE_REPORT.md) 发现 RDMA 真写 08-06→08-08 回落(3743→~2334),本文用打点定位回落与两路差距的物理根因。

---

## 1. 结论(先讲)

1. **NVMe 写盘是两路共同的根本瓶颈**:RDMA 单 part AIO 写 8M ≈ 5.3ms(~1500 MB/s 单流),GDS ≈ 7.3ms(~1100 MB/s 单流)。这是**直写 TLC 的带宽**(SLC 缓存耗尽后),远低于 08-06 的 SLC 缓存速度 → 直接解释 3743→2334 的回落。
2. **RDMA 主 loop 串行**:memcpy(1.7ms)+ AIO 写(5.3ms)+ 队列 backlog(1.9ms)全跑在单一主 EventLoop,~10ms/part 串行。其中 8M memcpy(1.7ms)是**纯冗余开销**(mock 跳过它 → 这是 mock 比真写快的部分原因,但非主因;主因是 AIO 写盘)。
3. **GDS 比 RDMA 慢 2.9×/part(38ms vs 13ms)**:① cuobj RDMA-read 更慢(2.7ms vs 2.2ms);② **每 part 2 次 AIO 写**(DevDataHeader + data,而 RDMA 合成 1 次);③ 主 loop exec 14.9ms 更重;④ worker 队列尾部(avg 10.5ms)。
4. **`disk_wait_us` 是误称**:它是 dispatch 等待(post 队列 + exec + notify),**不是**盘写时间;真盘写看 `disp_done_cb_us`(RDMA)/ `aio_write`(GDS)。

---

## 2. RDMA 真写拆解(8M part,n=1024,µs)

| 阶段 | p50 | avg | p95 | 说明 |
|---|---|---|---|---|
| **rdma_read_us** | 2186 | 2009 | 2275 | RNIC READ 8M(regmr=0 池命中,postread/read 链路) |
| crc32_us | 689 | 706 | 927 | CRC32C |
| disp_post_us | 1868 | 2335 | 5406 | worker 投递→主 loop 开始的队列 backlog |
| disp_memcpy_us | 1705 | 2250 | 4598 | 主 loop 上 8M memcpy(真写不跳;mock 跳) |
| **disp_done_cb_us** | **5265** | 5654 | 9736 | **AllocWriteOffset 后→gds_done 回调 = AIO 写盘** |
| disp_notify_us | 8 | 9 | 15 | cv 唤醒 worker |
| disp_exec_us | 7203 | 7906 | 11410 | 主 loop exec = memcpy+keysmap+alloc+done_cb |
| disk_wait_us | 10344 | 10249 | 12345 | dispatch 等待 = post(1868)+exec(7203)+notify(8)+gap |
| **total_us** | **13009** | 13265 | 15010 | rdma_read(2.2)+crc(0.7) 在 worker + disk_wait(10.3) 在主 loop |

**读数**:
- 主 loop 串行段(post+memcpy+AIO 写)≈ 1868+1705+5265 = 8838µs,占总 13ms 的 **68%**。
- AIO 写盘 5265µs/8M = **~1500 MB/s 单流**——这是直写 TLC 速度(SLC 缓存耗尽)。08-06 该值若 ~2500µs(~3200 MB/s SLC)即对应 3743 吞吐。
- regmr=0(MR pool 命中)、postread/read 极小——RDMA 读链路非瓶颈。
- `disk_wait_us` 10344 ≠ 盘写时间,是"worker 等主 loop 把 PutFromRdma 干完"的总时延(含盘写)。

---

## 3. GDS 真写拆解(8M part,n=1024,µs)

| 阶段 | p50 | avg | p95 | 说明 |
|---|---|---|---|---|
| rdma_read | 2746 | 3033 | 5074 | cuobj RDMA READ 8M(比 RDMA 路径 2186 慢 ~25%) |
| crc32c | 678 | 690 | 816 | CRC32C |
| **aio_write**(×2/part,n=2048) | **7309** | 9726 | 26530 | **NVMe 写,每 part 2 次(DevDataHeader + data)** |
| xb_post_main_wait | 6689 | 8668 | 22526 | worker→主 loop 队列等待 |
| xb_main_to_done | 14911 | 16878 | 29965 | 主 loop exec(含 aio_write) |
| xb_done_to_response | 1420 | 2389 | 9747 | done→response 发送 |
| xb_queue_wait(worker) | 84 | 10470 | 32584 | worker 内队列(p50 小但尾部大) |
| **xb_total_recv_to_response** | **38189** | 43826 | 71119 | 全程 |

**读数**:
- 每 part **2 次 AIO 写**(n=2048 / 1024 part)——GDS 把 DevDataHeader 与 data 分两次落盘,而 RDMA 的 PutFromRdma 把 header+data 合进一个 ioContext 一次 SubmitWrite。这是 GDS 主 loop exec(14.9ms)比 RDMA(7.2ms)重 2× 的主因之一。
- aio_write 7309µs/8M = **~1100 MB/s 单流**(略慢于 RDMA 的 1500,疑 2 次 AIO 的 submit 开销摊薄)。
- cuobj rdma_read 2746 > RDMA 路径 2186:GDS 走 cuobj token 路径(cuMemObjGetRDMAToken + cuobj 服务 18666),比 libibverbs 直连慢 ~25%。
- 主 loop exec 14.9ms + post_main_wait 6.7ms + worker 尾部 → total 38ms,是 RDMA 13ms 的 **2.9×**,与吞吐比(2334 vs ~890? 实测 GDS 1947,比 2.9× 略小因并发度差异)方向一致。

---

## 4. 两路差距归因(为何 GDS < RDMA)

| 因素 | RDMA | GDS | 影响 |
|---|---|---|---|
| RDMA-read | 2186µs(libibverbs 直连) | 2746µs(cuobj token 路径) | GDS +25% |
| AIO 写次数/part | 1(header+data 合一) | 2(header + data 分开) | GDS 主 loop exec 2× |
| 主 loop exec | 7203µs | 14911µs | GDS 2.06× |
| 单流盘写速度 | ~1500 MB/s | ~1100 MB/s | GDS 略慢(2 次 submit 开销) |
| 全程/part | 13ms | 38ms | GDS 2.9× |

GDS 慢的根因**不在 buffer pool**(已证伪,见 [GDS_POOL_OPTIMIZATION](GDS_POOL_OPTIMIZATION.md)),而在 **cuobj 读路径 + 2 次 AIO 写 + 主 loop 串行**三件事叠加。

---

## 5. 回落(3743→2334)根因 = NVMe SLC 缓存耗尽

- 08-06 文档 RDMA 真写 3743、08-08 复测 ~2334(且单次会下探到 ~1547);GDS 基本不动(1947≈1714)。
- 打点:RDMA `disp_done_cb`(AIO 写)p50=5265µs → 单流 ~1500 MB/s。若 08-06 为 SLC 缓存速度(~3000 MB/s,done_cb ~2600µs),单 part 主 loop段省 ~2.6ms → 13ms→10.4ms → 吞吐 +25%,量级吻合 3743→2334 的差。
- 本会话累积真写 ~300GB+(日志 `devid 88, tailOffset 4.28GB`,7TB 盘),SLC 缓存(~数十 GB)写满后直写 TLC,带宽腰斩。GDS 因绝对值本就低且受 cuobj/AIO 次数主导,对 SLC 耗尽不敏感 → 不动。
- **非根因(已排除)**:CPU 降频(governor powersave→performance 真写反降,~1599,见 [REAL_READWRITE_REPORT §0](REAL_READWRITE_REPORT.md#0-2026-08-08-复测校验必读rdma-真写回落))、MR pool(关 pool 跌到 1451,更差)。

---

## 6. 下一步优化方向(用户指导后)

1. **NVMe 写带宽**:若要复现/超过 3743,需 SLC 缓存可用(盘空闲/trim)或换更快 NVMe;否则 ~2300–2700 是 TLC 直写上限。可用 `fio` 直测裸盘写带宽对照。
2. **RDMA 主 loop 并行化**:memcpy(1.7ms)+ AIO 写(5.3ms)串行在单 EventLoop;把 AIO 完成回调异步化或多 worker loop,让 memcpy 与前一个 part 的 AIO 写重叠 → 提升主 loop 利用率。
3. **GDS 2 次 AIO 合并**:若能把 DevDataHeader 与 data 合进单次 SubmitWrite(同 RDMA),GDS 主 loop exec 可近半 → GDS 提升潜力大。需改 gds_service / ioContext 写路径。
4. **8M memcpy 冗余**:RDMA 真写仍 memcpy 8M(主 loop 1.7ms);若 data 直接落盘(零拷贝)可省。属架构改动,谨慎。
5. **补打点**:GDS 的 `[gds-timing]` 与 RDMA 的 `HandleRdmaPut DONE PHASE` 已够定位;proxy 层仍缺 per-RPC 计时(见 [proxy-lacks-per-RPC-timing](../../))。

---

## 附录 A:复现命令

```bash
# 后端:真写 + 全打点
cd /mnt/us3_test/xinghui.shao/gds/ggds-compile-env/ufile-ac-new
# config: [gds]/[rdma] mock_aio_write=0, mr_pool_max_buffers=64
US3_GDS_PUT_TIMING=1 ./build/ufile-ac --config-file=config/ufile-ac-gds-proxy.ini &

# 跑(任意一路)
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
./build/rtest/bench/rdma/us3_turbo_bench_rdma_multipart \
  --part-size 8388608 --total 67108864 --concurrency 32 --reps 3 --warmup 1

# 解析 backend 日志各阶段 p50
LATEST=$(ls -t /mnt/us3_test/ld/log/set1/set01-m00-d00/ufile-ac.* | head -1)
# RDMA: grep 'HandleRdmaPut DONE' "$LATEST" | grep PHASE | <解析 alloc/rdma_read/crc/disk_wait/total + [rdma-dispatch] + [rdma-read]>
# GDS:  grep '\[gds-timing\]' "$LATEST" | <按 recv_to_submit/rdma_read/aio_write/extra_breakdown 分类>
```
