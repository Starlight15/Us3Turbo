# GDS buffer pool 扩容优化(负结果)

**测试日期**:2026-08-06
**结论**:**扩容 pinned buffer pool 反而使 GDS 性能下降 ~27%。原始配置 `kBufferMaxPerClass=4`、
`BufferSizeClasses={1M,16M}` 已是最优。pool 不是 GDS 瓶颈。**

---

## 1. 背景与假设

真实读写 multipart GDS(8M part / 64M / 32 并发)吞吐 ~1714 MiB/s,vs RDMA ~3743(2.18× 差距)。
此前(`docs/PART_SIZE_DEEP_ANALYSIS.md`)据 16M part 时 setup p50 暴涨、单独加 nt/cp 反降等
曲线,推断瓶颈是后端 `gds_service.cc` 的 pinned buffer pool:

- `kBufferMaxPerClass=4`(每档最多缓存 4 个 buffer)
- `BufferSizeClasses={1M,16M}`(无 8M 档,8M part 取整到 16M 档)
- 假设:32 并发下 4 个 buffer 成争抢点,大量请求走 `allocHostBuffer+registerBuffer` 慢路径 → 扩容应提升

## 2. 方法

`ggds-compile-env/ufile-ac/gds_service.cc` 临时加 `US3_GDS_POOL_MAX_PER_CLASS` 环境变量覆盖
`max_per_class`(不重编扫描),`BufferSizeClasses` 试 {1M,16M} 与 {1M,8M,16M} 两组。每配置重启
ufile-ac(真实读写)跑 `us3_turbo_bench_gds_multipart --part-size 8M --total 64M --concurrency 32
--reps 10 --warmup 2` 多轮。proxy 不变(nt16/cp16/part8M)。

**实验后代码已 revert 回原始**(`kBufferMaxPerClass=4`、`{1M,16M}`);环境变量仅实验期临时存在。

## 3. 结果

### 3.1 max_per_class 扫描(16M 档,原始 buffer 大小)

| max_per_class | 吞吐 (MiB/s) | 备注 |
|---|---|---|
| 1 | ~1445 | 1 buffer 争抢过重 |
| 2 | ~1654 | |
| **4** | **~1667** | **原始值,峰值** |
| 8 | ~1508 | |
| 32 | ~1241 | **−27%** |
| 64 | ~1207 | |

倒 U 形,峰值在 4。扩容单调下降。

### 3.2 8M 档(加 8M class)+ max=32

~1240,与 16M 档 max=32 一致 → 回归来自 **max_per_class 增大**,与 8M/16M buffer 选择无关。

### 3.3 清基线对照

为排除"重编本身改变行为",把 `gds_service.cc` revert 到原始后重编:~1693(与扩容前 1714 一致,
重编非混淆因素)。故 **回归确由 max_per_class 增大导致**。

## 4. 机制推断(为何更大 pool 更慢)

- max=4:32 并发下仅 4 个命中 cache,其余 28 走**每次 fresh** `allocHostBuffer+registerBuffer`,
  Release 时 free_list 已满(≥4)→ 立即 Dispose(free+deregister)。即每 part 一次 alloc+register+dispose 抖动。
- max=32:全部 32 命中 cache,**复用** cached MR/buffer。

数据表明:**复用 cached MR 比 fresh alloc+register 慢 ~27%**。机制疑点(未深究):复用 buffer 的页表/
TLB 局部性、或 cuObjServer 对复用 MR 的 RDMA READ 路径有隐藏串行;fresh `allocHostBuffer` 可能取到
更优对齐/巨页区域利于 NVMe DMA。确切机制需进一步埋点(`US3_GDS_PUT_TIMING` 已有 alloc/pool/rdma_read
分段计时可用)。

## 5. 结论

1. **pool 配置已最优**(max=4,{1M,16M}),勿扩容。扩容至 32/64 使 GDS 降 27%。
2. 原"buffer pool 争抢是 GDS 瓶颈"的假设**证伪**。GDS 与 RDMA 的 2.18× 差距**不在 pool**。
3. GDS 真实瓶颈在别处:cuObj token 获取 / RDMA READ client GPU 显存 / GDS worker 串行(后端
   `worker_threads=8`) / NVMe 落盘。需另做 per-stage 计时定位。
4. 此前 `docs/PART_SIZE_DEEP_ANALYSIS.md` 中"16M part setup p50 暴涨归因 buffer pool 4 个"的推断
   需修正:那更可能是 16M 单 part 下 cuObj/NVMe 路径开销,非 pool 容量。

## 6. 环境告警

测试中发现测试机根盘 **100% 占满**(93G/98G,仅剩 109M),ufile-ac 数据落在该 LV。这导致真实写
RDMA multipart 从会话早期的 ~3743 跌至 ~2000(NVMe 无空闲空间、写放大)。GDS 因非 NVMe-bound
受影响较小(仍 ~1660)。**`docs/REAL_READWRITE_REPORT.md` 的 RDMA 3743 数据在盘满状态下不可复现**,
需先清理 ufile-ac 数据盘(`/opt/ufile/osd/set01-m00-d00` 及 binlog)再测方得稳定真实写数值。

## 7. 未做 / 后续

- 不改 pool(已证最优)。
- 下一步 GDS 优化应聚焦 cuObj token / RDMA READ / worker 串行,需先加 proxy per-RPC 计时
  (`memory/proxy-lacks-per-rpc-timing.md`)与 backend per-stage 计时精确定位。
- 清理数据盘后重测真实读写基线(当前盘满,数值不可信)。
