# GDS / RDMA 真实写入性能瓶颈深度分析(日志打点)

**日期**:2026-08-08(二稿,修正一稿两处定性错误)
**方法**:后端带 `US3_GDS_PUT_TIMING=1` 全打点,`mock_aio_write=0` 真落盘,最优配置 8M/16/16,binary 含 MR-pool commit 4fdd47a。跑 GDS+RDMA multipart 8M(conc32),解析 backend 日志各阶段 µs(n=1024/part,GDS 先跑 11:52:22、RDMA 后跑 11:52:35,同 dev:89)。
**承接**:[REAL_READWRITE_REPORT](REAL_READWRITE_REPORT.md) 发现 RDMA 真写回落(3743→~2727)。本文用打点定位物理根因。

> **一稿勘误(本稿修正)**:一稿称"主 loop 串行段含 AIO 写 ≈8.8ms 占总 68%""GDS 每 part 2 次 AIO 写"。两处均**错误**:
> - AIO 写是**真异步**(`io_submit`+`eventfd`+非阻塞 `io_getevents`,见 `ustevent/src/libevent/aio_util.cc`),`done_cb_us ≈ aio_write_us`(gap p50=**3µs**)→ done_cb 几乎 100% 是异步盘写延迟,**完成侧主 loop CPU≈0、主 loop 不被盘写阻塞**。
> - GDS 与 RDMA **都是 1 次 data AIO/part**(1024 part → 1024 aio_write 行,`WriteData` 单次 `SubmitWrite` 写 header+data 连续块),非 2 次。

---

## 1. 结论(先讲,修正版)

1. **AIO 写盘是真异步,主 loop 不被盘写阻塞。** `done_cb_us` p50=5275 ≈ `aio_write_us` p50=5272,**gap(完成侧 HintWriteData+回调 CPU)p50=3µs/avg 5µs**。即从 `AllocWriteOffset` 之后到 `gds_done` 回调的 5.27ms **几乎全是异步 NVMe 写延迟**,主 loop 在此期间处理别的 part,**不串行等盘**。这推翻了一稿"主 loop 串行段含 AIO 写占 68%"。
2. **主 loop 每 part 唯一显著 CPU = memcpy**(RDMA 1.7ms / GDS 0.95ms;malloc/keysmap/alloc 全 ~0)。主 loop 提交上限 = 8M/memcpy:RDMA ~4700 MB/s、GDS ~8400 MB/s,**均 > 实测吞吐(RDMA 2727 / GDS 1775)→ 主 loop CPU 不是吞吐瓶颈**。`post_us` 队列 backlog p50=1.88ms ≈ 1 个 part 缓冲(mild,非深度饱和)印证主 loop 有余力。
3. **真瓶颈 = NVMe 聚合写带宽。** RDMA 2727 ≈ 单流 TLC 1500 MB/s(p50 5.27ms/8M)× ~1.8 并发;**同 run 内 aio_write 双峰**:15.8% SLC 速(<2500µs,>3.3 GB/s)、80.3% TLC 直写(>4000µs)、31.4% 尾部 >10ms。SLC 缓存耗尽后直写 TLC ~1500 MB/s,直接解释 3743→2727 回落(08-06 SLC 可用时更快)。
4. **GDS 慢 2.9×/part(38ms vs 13ms)的真因不是"2 次 AIO"**,而是:① cuobj RDMA-read 2.75ms vs 2.19ms(+25%);② **GDS 单次 AIO 写 p50=11.2ms = RDMA 5.27ms 的 2.1×**(min 同为 ~2ms SLC 地板,差异在 p50/尾部)——GDS 8 worker 提交更快→NVMe 在途队列更深→单流完成延迟膨胀(GDS 59.8% AIO >10ms vs RDMA 31.4%);③ worker 内 `queue_wait` 尾部爆(p95 32.6ms / max 45.8ms,8 worker 争抢)。
5. **GDS 过提交假设**:GDS 把 NVMe 队列深度推过最优区→聚合带宽反降(GDS 1775 < RDMA 2727,尽管 GDS 并行度更高)。可测:减 `gds worker_threads` 8→4/2,看 per-AIO 延迟是否回落、聚合是否升。
6. `disk_wait_us`(RDMA 10344)= dispatch 等待(post 队列+exec+notify)= worker 等主 loop 把 PutFromRdma 干完的墙钟,含异步 AIO;**非盘写时间**。真盘写看 `aio_write_us`。

---

## 2. 决定性证据:done_cb ≈ aio_write(gap=3µs)

按 key 关联 `[rdma-dispatch] done_cb_us`(t_after_alloc→t_gds_done)与 `[gds-timing] aio_write us`(t_disk_submit→WriteDeviceDone 入口),n=1024:

| 量 | p05 | p25 | p50 | p75 | p95 | avg | min | max |
|---|---|---|---|---|---|---|---|---|
| done_cb_us | 2042 | 2280 | **5275** | 7663 | 9739 | 5654 | 1974 | 13731 |
| aio_write_us | 2039 | 2253 | **5272** | 7662 | 9728 | 5648 | 1970 | 13727 |
| **gap(done−aio)=完成侧 CPU** | — | — | **3** | — | 17 | 5 | 1 | 185 |

`done_cb = (alloc→t_disk_submit 提交侧小 CPU) + aio_write(异步盘写) + (WriteDeviceDone→gds_done 完成侧 CPU)`。gap p50=3µs → **提交侧+完成侧主 loop CPU 合计仅 3µs,5.27ms 全是异步盘写**。证:AIO 不阻塞主 loop。

> 机制(`ustevent/src/libevent/aio_util.cc`):`SubmitWrite`→`io_submit(ctx,1,&piocb)`+`io_set_eventfd` 即返回(不阻塞);完成时 eventfd 可读→libevent `ReadEventCb`→`SomethingDone`→`io_getevents(ctx_,0,count,events,&timeout={0,0})` 非阻塞收割→`aio_data->cb`(WriteDeviceDone)→HintWriteData→gds_done_cb_。主 loop 在 AIO 在途期间跑别的 part 的 memcpy+submit。

---

## 3. RDMA 真写拆解(8M,n=1024,µs)

| 阶段 | p05 | p50 | p95 | avg | 说明 |
|---|---|---|---|---|---|
| rdma_read_us(worker) | — | 2186 | 2275 | 2009 | RNIC READ 8M(regmr=0 池命中) |
| crc32_us(worker) | — | 689 | 927 | 706 | CRC32C |
| post_us(队列 backlog) | 7 | **1877** | 5422 | 2334 | worker QueueInLoop→PutFromRdma 开始;≈1 part 缓冲 |
| malloc_us | 0 | 0 | 0 | 0 | ioContext pool malloc |
| **memcpy_us(主 loop CPU)** | 1260 | **1715** | 4598 | 2249 | 8M MR→data_ 拷贝,**主 loop 唯一显著 CPU** |
| keysmap_us | 0 | 1 | 4 | 0 | keysmap Get |
| alloc_us | 0 | 0 | 0 | 0 | AllocWriteOffset |
| done_cb_us | 2042 | **5275** | 9739 | 5654 | ≈ aio_write(异步盘写),完成侧 CPU≈0(见 §2) |
| notify_us | 6 | 8 | 15 | 8 | cv 唤醒 worker |
| exec_us(墙钟) | 3918 | 7203 | 11412 | 7906 | =sync CPU(memcpy 等)+异步 AIO+completion,**非纯 CPU** |
| disk_wait_us | — | 10344 | 12345 | 10249 | dispatch 等待(含异步 AIO),非盘写 |
| **total_us** | — | **13009** | 15010 | 13265 | worker(2.87ms)+主 loop 等待(10.3ms,含异步 AIO) |

**aio_write 双峰(n=1024,RDMA)**:

| 桶 | 占比 | 含义 |
|---|---|---|
| <2000µs(>4 GB/s) | 0.0% | — |
| 2000–3000µs(2.7–4 GB/s) | 17.4% | SLC 缓存命中 |
| 3000–4000µs | 2.2% | 过渡 |
| 4000–6000µs(1.4–2 GB/s) | 20.7% | TLC 直写 |
| 6000–8000µs | 14.3% | TLC+轻争抢 |
| 8000–10000µs | 13.9% | 争抢 |
| >10000µs | 31.4% | 深队列争抢尾部 |

SLC 速(<2500µs):15.8% / TLC 直写(>4000µs):80.3%。min 1970µs(~4 GB/s SLC 地板)→ p50 5272µs(~1.5 GB/s TLC)→ max 13731µs。

**读数**:主 loop 每 part 实际 CPU = memcpy 1.7ms(+~0)。提交上限 8M/1.7ms≈4700 MB/s > 实测 2727 → 主 loop 非瓶颈。NVMe 聚合写(~2700,~1.8 单流)是 cap。post_us 1.88ms = ~1 part 缓冲,主 loop 近不饱和。

---

## 4. GDS 真写拆解(8M,n=1024,µs,本 run 先于 RDMA)

| 阶段 | p05 | p50 | p95 | avg | 说明 |
|---|---|---|---|---|---|
| rdma_read(worker) | 1286 | **2748** | 5091 | 3032 | cuobj token 路径(cuMemObjGetRDMAToken+cuobj 服务 18666),比 libibverbs 直连慢 25% |
| crc(worker) | — | ~580 | — | — | CRC32C |
| memcpy(主 loop) | 871 | **954** | 1240 | 982 | 比 RDMA 1715 小(拷贝路径不同,非 2×) |
| queue_wait(worker 内) | 5 | 85 | **32587** | 10470 | 8 worker 争抢,p50 小但尾部爆(max 45760) |
| post_main_wait(队列 backlog) | 577 | **6710** | 22528 | 8667 | worker→主 loop 队列等待 |
| main_to_done | 6106 | **14912** | 29966 | 16877 | 主 loop 墙钟(含异步 AIO 11.2ms + memcpy + completion) |
| done_to_response | 32 | 1429 | 9748 | 2388 | done→response |
| **total_recv_to_response** | 19179 | **38192** | 71123 | 43826 | 全程 |

**GDS aio_write(n=1024,1/part,非 2)**:p50=**11224**µs、p95=28413、max=33486、min=2026。双峰:SLC 速(<2500µs)4.6% / TLC 直写(>4000µs)91.0% / **>10ms 占 59.8%**(深队列争抢尾部远重于 RDMA 的 31.4%)。

**读数**:
- GDS **1 次 data AIO/part**(1024 part→1024 aio_write 行,`WriteData` 单次 `SubmitWrite` 写 header+data 连续块),一稿"2 次 AIO/part"错。
- GDS 单次 AIO 写 p50 11.2ms = RDMA 5.27ms 的 **2.1×**,但 min(2026≈1970)同 SLC 地板 → 差异不在盘态,在**队列深度**:GDS 8 worker 提交更快→NVMe 在途更深→单流完成延迟膨胀(59.8% >10ms vs RDMA 31.4%)。
- `queue_wait` worker 内尾部 p95 32.6ms/max 45.8ms:8 worker 在 cuobj 读+AIO 完成回环里争抢,尾部爆。
- memcpy 954 < RDMA 1715(路径不同),非 GDS 慢因。
- total 38ms = rdma_read 2.75(worker)+post_main_wait 6.7(队列)+main_to_done 14.9(含异步 AIO 11.2)+done_to_response 1.4+queue_wait 尾部(worker,部分重叠)。

---

## 5. 两路差距归因(修正版)

| 因素 | RDMA | GDS | 影响 |
|---|---|---|---|
| AIO 写次数/part | 1 | **1(非 2,一稿错)** | 持平 |
| cuobj vs libibverbs 读 | 2186µs | 2748µs | GDS +25% |
| **单次 AIO 写延迟** | 5272µs p50 | **11224µs p50(2.1×)** | GDS NVMe 队列更深→单流带宽腰斩 |
| AIO >10ms 占比 | 31.4% | 59.8% | GDS 争抢尾部更重 |
| worker queue_wait 尾部 | (4 worker,p95 小) | p95 32587/max 45760 | GDS 8 worker 争抢爆 |
| memcpy(主 loop CPU) | 1715µs | 954µs | GDS 反而更小 |
| 主 loop 提交上限 | ~4700 MB/s | ~8400 MB/s | 均 >实测,非瓶颈 |
| 全程/part | 13ms | 38ms | GDS 2.9× |
| 实测吞吐 | 2727 | 1775 | GDS 更低 |

**GDS 慢的真因(修正)**:不是"2 次 AIO 写",而是 **cuobj 读慢 + NVMe 写队列被 GDS 过深提交→单流 AIO 延迟 2.1× + worker 8 路争抢尾部爆**。GDS 把更多并行度推给同一块 NVMe,队列深度越过最优区→聚合带宽反降(1775 < 2727)。

---

## 6. 回落(3743→2727)根因 = NVMe SLC 缓存耗尽

- 08-06 RDMA 真写 3743、08-08 复测 ~2727(-27%);GDS 基本不动(1775≈1714)。
- 打点:RDMA aio_write p50=5272µs/8M ≈ 1500 MB/s(TLC 直写);min 1970µs(~4 GB/s SLC 地板)。若 08-06 SLC 可用(aio_write ~2500µs ~3200 MB/s),单 part 异步段省 ~2.7ms → 吞吐 +25%,量级吻合 -27%。
- 同 run 内 aio_write 双峰(15.8% SLC / 80.3% TLC)即"SLC 边写边耗尽"的运行内证据。GDS 因绝对值低 + 受 cuobj/队列争抢主导,对 SLC 不敏感 → 不动。
- **非根因(已排除)**:CPU 降频(governor performance 真写反降 ~1599)、MR pool(关 pool 跌到 1451)、主 loop 串行(AIO 异步,gap=3µs)。

---

## 7. 下一步优化方向(用户指导后,修正版)

1. **NVMe 写带宽(根本 cap)**:~2700 是 TLC 直写上限。要复现/超 3743 需 SLC 可用(盘空闲/trim)或更快盘;`fio` 直测裸盘写带宽+队列深度曲线对照。**最高优先**。
2. **GDS 提交节流(已验证,见效)**:减 `[gds] worker_threads` 8→4,GDS 吞吐 **1775–1894 → 2300–2368 MiB/s(+21–33%)**,aio_write p50 11.4ms→6.0ms,>10ms 尾部 60%→7%。机理:GDS 8 worker 过提交→NVMe 在途队列深度越过最优区→单流 AIO 延迟 2× + 聚合反降。wt=4 与 RDMA(wt=4)在途数一致→per-AIO 趋同(6.0 vs 5.3ms)。**见 §8 扫描数据**。wt=4 为最优点(非单调曲线:wt=2 worker-bound 1594、wt=6 已回升争抢 1828)。剩余 GDS<RDSA 16% 差距=cuobj 读(2.7 vs 2.2ms)+ aio 略高。**建议把 `[gds] worker_threads` 默认 8→4(零代码改动,可立即落地)**。
3. **memcpy 零拷贝(降级,次优先)**:RDMA memcpy 1.7ms 现非 cap(4700>2727),但若 NVMe 回到 SLC 速(~4000),memcpy 将成新 cap。改 `SubmitWrite` 直接从 RDMA MR buffer 写(跳 data_ 中转)。架构改动,谨慎。
4. ~~主 loop 并行化~~(一稿建议,降级):AIO 已异步、主 loop 不被盘写阻塞、post backlog 仅 ~1 part → 主 loop 有余力,**并行化收益不如一稿估计**。仅在 NVMe 提速后 memcpy 成 cap 时才有意义。
5. **GDS cuobj 读路径**:2748 vs 2186,token 路径结构慢;长期可改直连,但收益小(+25% 读段,非主因)。
6. **补打点**:proxy 层仍缺 per-RPC 计时(见 [proxy-lacks-per-RPC-timing](../../));backend 侧 [gds-timing]/[rdma-dispatch]/[rdma-read] 已够定位。

---

## 8. GDS worker_threads 扫描(已验证,2026-08-08)

承接 §1 结论 4/5(GDS 过提交假设),扫 `[gds] worker_threads ∈ {2,4,6,8}`,各重启后端带 `US3_GDS_PUT_TIMING=1`,GDS multipart 8M conc32 reps3 warmup1,解析 aio_write p50 + 双峰 + 吞吐:

| gds wt | 吞吐 (MiB/s) | aio_write p50 (µs) | aio >10ms 尾部 | SLC% | 机理 |
|---|---|---|---|---|---|
| 2 | 1594 | 4166 | 0% | 49% | 2 在途无争抢(per-AIO 最优),但 2 worker 喂不饱→worker-bound |
| **4(峰)** | **2300–2368** | **6008–6100** | ~7% | 25% | 在途≈RDMA(wt=4),per-AIO 趋同(6.0 vs RDMA 5.3);sweet spot |
| 6 | 1828 | 10710 | 高 | 16% | 争抢回升,per-AIO 翻倍 |
| 8(默认) | 1775–1894 | 11224–11399 | 60% | 4–? | 8 worker 过提交→NVMe 队列越过最优区→聚合反降 |

**关键证据**:
- **非单调曲线**(wt=4 处峰,wt=6/8 反降):wt=8 把更多并行度推给同一 NVMe,队列深度越过最优区→单流 AIO 延迟 2×(11.4 vs 6.0ms)+ 尾部爆(60% vs 7%)→聚合带宽反降 21–33%。"过提交适得其反"。
- **争抢效应独立于 SLC 态**:wt=4 fresh(25% SLC)与 wt=8 fresh(4% SLC)对比,wt=4 SLC 占比更高(本应更快)但其 per-AIO 6.0ms 仍远低于 wt=8 11.4ms;同 TLC 桶 wt=4 ~6ms vs wt=8 ~11ms,2× 来自队列深度而非盘态。
- **wt=4 = RDMA 同在途数**:RDMA `worker_threads=4`→aio 5.27ms/吞吐 2727;GDS wt=4→aio 6.0ms/吞吐 2300。GDS 仍低 16%(cuobj 读 2.7 vs 2.2ms + aio 略高),非在途数差异。
- **wt=2 worker-bound**:per-AIO 4.2ms(最佳,无争抢)但 2 worker × 8M/~13ms ≈ 1600,worker 数成 cap。

**结论**:`[gds] worker_threads` 默认 8→4,GDS 真写吞吐 +21–33%(~1894→~2300),per-AIO 延迟 -47%(11.4→6.0ms),争抢尾部 -53pp。零代码改动(仅配置),可立即落地。

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

# 解析(按 key 关联 done_cb↔aio_write 决定性拆分)
LATEST=$(ls -t /mnt/us3_test/ld/log/set1/set01-m00-d00/ufile-ac.* | head -1)
grep '\[rdma-dispatch\]' "$LATEST" | sed -E 's/.*key:([^ ]+).*done_cb_us=([0-9]+).*/\1 \2/'   # done_cb
grep 'aio_write us='    "$LATEST" | sed -E 's/.*aio_write us=([0-9]+) key:([^ ]+).*/\2 \1/'  # aio_write
# join on key → gap=done_cb-aio_write(应≈0,证 AIO 异步)
```

## 附录 B:关键源码定位

- `rdma_service.cc:438` `HandleRdmaPut DONE PHASE`(alloc/pool/rdma_read/crc/disk_wait/total)
- `rdma_service.cc:448` `[rdma-dispatch]`(post/exec/notify + exec_breakdown malloc/memcpy/keysmap/alloc/done_cb)
- `rdma_service.cc:462` `[rdma-read]`(regmr/postread/read)
- `ac_server.cc:1727` `PutFromRdma`(主 loop:malloc+memcpy+keysmap+AllocWriteOffset+WriteData→异步 SubmitWrite)
- `ioContext.cc:412` `WriteData`→`SubmitWrite(...,WriteDeviceDone,this)`(异步)
- `ioContext.cc:460` `WriteDeviceDone`→`HintWriteData`→`gds_done_cb_`(完成回调)
- `ustevent/src/libevent/aio_util.cc:50` `AioUtil::SubmitWrite`(`io_submit`+`io_set_eventfd`,真异步)
- `ustevent/src/libevent/aio_util.cc:97` `SomethingDone`(eventfd→非阻塞 `io_getevents`→`cb`)
