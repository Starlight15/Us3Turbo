# Us3Turbo 真实读写性能测试报告

**测试日期**:2026-08-06(初测)/ 2026-08-08(复测,见 §0)
**测试目标**:在最优配置(`part=8M / num_threads=16 / backend_conn_pool_size=16`,见
三个 `*_DEEP_ANALYSIS.md`)下,关闭 backend mock,
测含 NVMe 持久化的真实吞吐;覆盖单步 PUT、分段上传、下载 GET 三类操作;并与
`--mock-aio-write=1`(纯数据搬运,无落盘)对照。

---

## 0. 2026-08-08 复测校验(必读:RDMA 真写回落)

同配置(8M/16/16,`mock_aio_write=0` 真落盘,binary 含 MR-pool commit 4fdd47a)各跑 3 轮:

| 项 | 原(08-06) | 复测(08-08) | Δ |
|---|---|---|---|
| 单步 RDMA PUT 4M | ~3258 | ~1821 | **-44%** |
| 分段 RDMA 8M | ~3743 | ~2727 | **-27%** |
| 单步 GDS PUT 4M | ~1273 | ~1224 | -4% ✓ |
| 分段 GDS 8M | ~1714 | ~1775 | +3.6% ✓ |
| GET RDMA 4M | ~2513 | ~2689 | +7% ✓(单轮方差大) |

**RDMA 真写两档均回落,GDS/GET 在会话方差内。** 诊断:关 MR pool(`mr_pool_max_buffers=0`)后 RDMA 分段真写反而跌到 ~1451(比开 pool 的 ~2727 更差)→ **MR pool 不是回落根因,它对真写仍净帮助**(省 per-req regmr+alloc);回落根因未定位(memory 记为"源码漂移",疑 NVMe / CPU 降频 / 设备状态——本机启动日志报 `CPU Frequency is not max (800≠3200)`)。GDS 不受影响(GDS 路径重 IO、轻 CPU/crc;RDMA 重 RDMA-read + memcpy + crc,对 CPU 降频敏感)。**本文以下 §2-§6 的 RDMA 数值已更新为 08-08 复测值;GDS/GET 同向小漂在噪声内,数值一并刷新。**

---

## 1. 测试环境

### 1.1 进程与端点

| 组件 | 端点 | 备注 |
|---|---|---|
| client (bench) | — | 本机进程 |
| us3_turbo_proxy | `192.168.1.198:9100` | brpc 控制面,最优 flag |
| ufile-ac | `192.168.1.198:24000` | 数据面,**mock 关闭,真落盘** |

### 1.2 固定参数

| 参数 | 值 | 说明 |
|---|---|---|
| backend mock | **关闭**(去掉 `--mock-aio-write=1`) | PUT 真落盘、GET 可读 |
| proxy | `num_threads=16 / conn_pool=16 / part_size=8M` | 调参最优(见 mock 报告) |
| client log_level | `warn` | 静默 INFO |
| proxy log_level | `warn` | 同上 |
| 轮数 | 每配置 3 轮取稳定值 | 多轮保证结果稳定 |
| warmup | 单步 PUT/GET=2,multipart=2 | 排除冷启动 |

### 1.3 启动命令

```bash
# 后端真实读写(去掉 --mock-aio-write=1)
cd /mnt/us3_test/xinghui.shao/gds/ggds-compile-env/ufile-ac
nohup ./build/ufile-ac --config-file=config/ufile-ac-gds-proxy.ini \
  > /tmp/uac_real.log 2>&1 &

# proxy 最优配置
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
nohup ./build/proxy/us3_turbo_proxy --flagfile=proxy/conf/proxy.flags \
  > /tmp/proxy.log 2>&1 &
```

### 1.4 bench 工具修复(本轮前)

测试前修复了 bench 两个 warmup 统计 bug:warmup 字节曾计入吞吐分子致虚高、GET
warmup 命中假 key 产假 fail;拆 `put_one`/`get_one` 后 warmup 不记账/不计时。
multipart bench 不受影响(warmup round 本就不进 stats)。**本轮所有数值均来自
修复后的二进制。**

---

## 2. 单步 PUT(size=4M,count=40,conc=8,warmup=2)

单步受 `max_single_put_bytes=4M` 限制,故 size=4M。

| 通路 | run1 | run2 | run3 | 稳定值 | 时延 p50 / avg (ms) |
|---|---|---|---|---|---|
| GDS | 1115 | 1526 | 1224 | **~1224–1526 MiB/s**(波动) | ~20 / ~24 |
| RDMA | 2088 | 1707 | 1821 | **~1821 MiB/s**(08-08 复测;08-06 原测 ~3258,回落 -44%,见 §0) | ~16 / ~16 |

ok=40 / fail=0。GDS 在小对象(4M)+ 8 并发下波动较大,与 GDS backend buffer pool
(2 档×4 个)争抢一致;RDMA 08-08 复测较 08-06 回落,根因未定位(MR pool 已排除,见 §0)。

> 单步 PUT 吞吐高于 multipart 的原因:单步 4M 对象走单次 RPC,无 Create/Complete
> 控制面往返与 part 拼接开销,且每对象只需一个 NVMe block;但受 4M 上限约束,
> 大对象必须走 multipart。

---

## 3. 分段上传(part=8M,total=64M,conc=32,reps=10,warmup=2)

| 通路 | run1 | run2 | run3 | 稳定值 | data-plane p50 (ms) | total p50 (ms) |
|---|---|---|---|---|---|---|
| GDS | 1773 | 1772 | 1780 | **~1775 MiB/s** | 1020 | 1120 |
| RDMA | 2685 | 2721 | 2773 | **~2727 MiB/s**(08-08 复测;08-06 原测 ~3743,回落 -27%,见 §0) | 659 | 735 |

fail=0 全程。GDS 与 08-06 原测(~1714)一致;RDMA 08-08 回落,根因未定位(MR pool 已排除,见 §0)。

---

## 4. 下载 GET(RDMA,size=4M,count=100,conc=8,warmup=2)

GET bench 先串行 PUT 全部对象再并发 GET 测读吞吐。对象上限 4M(单步播种)。
GDS 无 GET bench,仅测得 RDMA。

| run1 | run2 | run3 | 稳定值(中位) | 时延 p50 / avg (ms) |
|---|---|---|---|---|
| 3335 | 2378 | 2689 | **~2689 MiB/s**(08-08 复测,单轮方差大;08-06 原测 ~2513) | ~10.8 / ~11.4 |

ok=100 / fail=0。

---

## 5. 与 mock(纯搬运)对照

| 操作 | mock(无落盘,08-08 复测) | 真实读写(08-08) | 落盘开销 |
|---|---|---|---|
| 分段 GDS 8M | 3985 | 1775 | **−55%** |
| 分段 RDMA 8M | 6786 | 2727 | **−60%** |
| 单步 RDMA 4M | (未测) | 1821 | — |
| 下载 RDMA 4M | (mock 下不可用) | 2689 | — |

(mock 列为 nt16cp16 复测值,见 [PART_SIZE §0](PART_SIZE_DEEP_ANALYSIS.md#0-2026-08-07-复测校验必读rdma-16m-结论已反转)。)
落差即 NVMe 持久化写入开销:mock 下 PUT 跳过 `SubmitWrite`(见
`ioContext.cc:424`),只测 `client → proxy → backend RDMA-read` 搬运段;真实写多了
落盘。**mock 报告适用于数据搬运路径调参;本报告为含持久化的端到端真实吞吐。**
两者均有效,场景不同。RDMA 真写较 08-06 原测回落(3743→2727,见 §0);GDS 落差比例稳定 ~55%。

---

## 6. 结论

1. 最优配置(8M/16/16)在真实读写下:GDS 分段 ~1775、RDMA 分段 ~2727 MiB/s(08-08 复测;08-06 原测 RDMA ~3743,回落 -27%,见 §0)。
2. RDMA 全程高于 GDS(分段 2727/1775≈1.53×、单步 1821/1224≈1.49×、下载仅测 RDMA),根因与 mock 报告一致:GDS 受 backend buffer pool(4 个 16M)争抢,RDMA 瓶颈在硬件带宽、无共享池争抢。
3. 真实写使吞吐降 55–60%(GDS −55%、RDMA −60%,08-08 复测),调参结论不变(最优点相同,绝对值按比例下降)。
4. 单步 PUT 吞吐高于 multipart(无控制面往返与拼接),但受 4M 上限,大对象必须分段。
5. **RDMA 真写 08-06→08-08 回落(-27% 分段 / -44% 单步)根因未定位**:已排除 MR pool(关 pool 更差,见 §0),疑 CPU 降频(启动日志 `CPU Frequency is not max 800≠3200`)/NVMe 状态;GDS 路径不受影响。后续可先确认 CPU 调速器(governor=performance)再复测。

---

## 7. 复现命令

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo

# 单步 PUT
./build/rtest/bench/gds/us3_turbo_bench_gds_put  --size 4M --count 40 --concurrency 8 --warmup 2
./build/rtest/bench/rdma/us3_turbo_bench_rdma_put --size 4M --count 40 --concurrency 8 --warmup 2

# 分段上传
./build/rtest/bench/gds/us3_turbo_bench_gds_multipart  --part-size 8M --total 64M --concurrency 32 --reps 10 --warmup 2
./build/rtest/bench/rdma/us3_turbo_bench_rdma_multipart --part-size 8M --total 64M --concurrency 32 --reps 10 --warmup 2

# 下载 GET(RDMA)
./build/rtest/bench/rdma/us3_turbo_bench_rdma_get --size 4M --count 100 --concurrency 8 --warmup 2
```
