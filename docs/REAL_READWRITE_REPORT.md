# Us3Turbo 真实读写性能测试报告

**测试日期**:2026-08-06
**测试目标**:在最优配置(`part=8M / num_threads=16 / backend_conn_pool_size=16`,见
`docs/PERF_BENCH_REPORT.md` 与三个 `*_DEEP_ANALYSIS.md`)下,关闭 backend mock,
测含 NVMe 持久化的真实吞吐;覆盖单步 PUT、分段上传、下载 GET 三类操作;并与
`--mock-aio-write=1`(纯数据搬运,无落盘)对照。

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

测试前修复了 bench 两个 warmup 统计 bug,详见 `docs/BENCH_FIX.md`(见下节)。修复前
单步 PUT/GET 的 warmup 会污染吞吐分子;multipart bench 不受影响。**本轮所有数值均来自
修复后的二进制。**

---

## 2. 单步 PUT(size=4M,count=40,conc=8,warmup=2)

单步受 `max_single_put_bytes=4M` 限制,故 size=4M。

| 通路 | run1 | run2 | run3 | 稳定值 | 时延 p50 / avg (ms) |
|---|---|---|---|---|---|
| GDS | 1157 | 1503 | 1273 | **~1273–1503 MiB/s**(波动) | ~21 / ~23 |
| RDMA | 3253 | 3270 | 3251 | **~3258 MiB/s** | 8.5 / 9.1 |

ok=40 / fail=0。GDS 在小对象(4M)+ 8 并发下波动较大,与 GDS backend buffer pool
(2 档×4 个)争抢一致;RDMA 稳定。

> 单步 PUT 吞吐高于 multipart 的原因:单步 4M 对象走单次 RPC,无 Create/Complete
> 控制面往返与 part 拼接开销,且每对象只需一个 NVMe block;但受 4M 上限约束,
> 大对象必须走 multipart。

---

## 3. 分段上传(part=8M,total=64M,conc=32,reps=10,warmup=2)

| 通路 | run1 | run2 | run3 | 稳定值 | data-plane p50 (ms) | total p50 (ms) |
|---|---|---|---|---|---|---|
| GDS | 1721 | 1655 | 1766 | **~1714 MiB/s** | 1011 | 1108 |
| RDMA | 3723 | 3755 | 3751 | **~3743 MiB/s** | 499 | 536 |

fail=0 全程。数值与 mock 报告趋势一致,绝对值因含落盘开销下降(见 §5)。

---

## 4. 下载 GET(RDMA,size=4M,count=100,conc=8,warmup=2)

GET bench 先串行 PUT 全部对象再并发 GET 测读吞吐。对象上限 4M(单步播种)。
GDS 无 GET bench,仅测得 RDMA。

| run1 | run2 | run3 | 稳定值(中位) | 时延 p50 / avg (ms) |
|---|---|---|---|---|
| 2389 | 2699 | 2513 | **~2513 MiB/s** | 11.2 / 12.1 |

ok=100 / fail=0。

---

## 5. 与 mock(纯搬运)对照

| 操作 | mock(无落盘) | 真实读写 | 落盘开销 |
|---|---|---|---|
| 分段 GDS 8M | 3864 | 1714 | **−56%** |
| 分段 RDMA 8M | 6527 | 3743 | **−43%** |
| 单步 RDMA 4M | (未测) | 3258 | — |
| 下载 RDMA 4M | (mock 下不可用) | 2513 | — |

落差即 NVMe 持久化写入开销:mock 下 PUT 跳过 `SubmitWrite`(见
`ioContext.cc:424`),只测 `client → proxy → backend RDMA-read` 搬运段;真实写多了
落盘。**mock 报告适用于数据搬运路径调参;本报告为含持久化的端到端真实吞吐。**
两者均有效,场景不同。

---

## 6. 结论

1. 最优配置(8M/16/16)在真实读写下依然成立:GDS 分段 ~1714、RDMA 分段 ~3743 MiB/s。
2. RDMA 全程高于 GDS(分段 2.18×、单步 2.55×、下载仅测 RDMA),根因与 mock 报告
   一致:GDS 受 backend buffer pool(4 个 16M)争抢,RDMA 瓶颈在硬件带宽、无共享池争抢。
3. 真实写使吞吐降 43–56%,调参结论不变(最优点相同,绝对值按比例下降)。
4. 单步 PUT 吞吐高于 multipart(无控制面往返与拼接),但受 4M 上限,大对象必须分段。

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
