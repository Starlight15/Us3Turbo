# Us3Turbo 分段上传性能测试报告

**测试日期**:2026-08-06
**测试环境**:client ↔ proxy(brpc 控制面)↔ ufile-ac(数据面)
**测试目标**:multipart PUT 的数据搬运吞吐,分析 proxy 三个参数(`--num_threads`、`--backend_conn_pool_size`、`--multipart_part_size`)对 GDS / RDMA 两条通路性能的影响,寻找最优配置。

---

## 1. 测试环境与固定条件

### 1.1 进程与端点

| 组件 | 端点 | 备注 |
|---|---|---|
| client (bench) | — | 本机进程,32 线程并发 |
| us3_turbo_proxy | `192.168.1.198:9100` | brpc 控制面 |
| ufile-ac | `192.168.1.198:24000` (TCP) / `:18666` (RDMA) | 数据面 |

### 1.2 固定参数(全测试不变)

| 参数 | 值 | 说明 |
|---|---|---|
| backend mock | `--mock-aio-write=1` | **PUT 不落盘**,只测数据搬运(client→proxy→backend RDMA-read),不含 NVMe 写;GET 不可用,故本轮只测 PUT |
| client 并发 | 32 线程 (`--concurrency 32`) | 全程固定 |
| total(单对象大小) | 64 MiB | 全程固定 |
| reps | 10 | 每配置每通路重复轮数 |
| warmup | 2 | 预热轮数,排除冷启动 |
| client log_level | `warn` | 静默 INFO 日志,避免污染时延 |
| proxy log_level | `warn` | 同上 |
| ufile-ac worker_threads | 8 (ini) | 固定 |
| ufile-ac `[rdma] mock_mode` | 0 | RDMA PUT 真写盘(mock 只影响 GDS aio_write,见下) |
| ufile-ac `[gds] mock_rdma_read` | 0 | GDS PUT 真 RDMA-read client 数据 |

### 1.3 代码改动(为支持 >4M part_size)

测试前移除了 part_size 的 4M 硬上限,改为可配置:

- **`rtest/bench/gds/gds_multipart_bench.cpp`** / **`rtest/bench/rdma/rdma_multipart_bench.cpp`**:
  - 4M 上限校验改为 16M 上限(`backend MAX_VALUE_LENGTH=16M`,见 `ufile-ac/entry.h:6`)
  - `ClientOptions` 透传 `--part-size` 到 `.multipart_part_size`(此前 bench 用默认 4M,传 8M/16M 会被 `client.cpp` 的 `exceeds multipart_part_size` 校验拒)
  - `ClientOptions.log_level = "warn"` 静默日志

- **`proxy/conf/proxy.flags`**:`--multipart_part_size` 已是 gflags,按测试组动态改值 + 重启 proxy。
- **backend (ufile-ac)**:**无需改代码**。单次 IO 上限 `MAX_VALUE_LENGTH=16M`(entry.h)已支持 16M;`in_max_size/out_max_size` 经核实为死代码(main.cc 计算后未使用,不限制接收)。

> ⚠️ 代码改动**已编译进 `build/`**,当前未回退。如需复现,bench 二进制已含上述改动。

### 1.4 测试方法

- 控制变量:每轮只改一个(或一组)proxy 参数,其余固定。
- 每改一次 proxy flag → `kill` 旧 proxy → `nohup` 重启 → 等待监听就绪 → 跑 bench。
- bench 输出 `throughput (MiB/s)`、`wall time (ms)`、阶段时延(setup / data-plane / control-plane 的 avg/p50/p95/min/max)、ok/fail 计数。
- **全部 0 fail**:本轮所有组 ok=320、fail=0。

---

## 2. 测试一:part_size 单参数扫描

### 2.1 配置

- 固定:`num_threads=8`、`backend_conn_pool_size=8`、client 32 线程、total=64M
- 扫描:`--multipart_part_size` ∈ {4M, 8M, 16M}
- 注:part_size 变化时 parts/round 随之变(64M / part_size)

### 2.2 结果

| part_size | parts/round | GDS throughput | GDS data-plane p50 | GDS wall | RDMA throughput | RDMA data-plane p50 | RDMA wall |
|---|---|---|---|---|---|---|---|
| 4M | 16 | 2407 MiB/s | 779 ms | 8508 ms | 3567 MiB/s | 511 ms | 5741 ms |
| **8M** | 8 | **3132 MiB/s** | 556 ms | 6540 ms | **6124 MiB/s** | 272 ms | 3344 ms |
| 16M | 4 | 1309 MiB/s | 1166 ms | 15650 ms | 3921 MiB/s | 384 ms | 5223 ms |

### 2.3 结论

**两条通路最优 part_size 均为 8M。**
- GDS:8M(3132)是 4M(2407)的 1.30×、16M(1309)的 2.39×。
- RDMA:8M(6124)是 4M(3567)的 1.72×、16M(3921)的 1.56×。

### 2.4 分析

**4M 偏低**:parts/round 多(16),每 part 一次 RPC + cuObj/libibverbs 注册-注销固定开销被摊到 4M payload 上,单 part 有效带宽低。8M 把开销摊薄一半,data-plane p50 下降(GDS 779→556ms,RDMA 511→272ms)。

**16M 劣化,两通路原因不同**:

1. **GDS 16M 崩到 1309(比 4M 还低)**:根因是 backend GDS buffer pool 配置——`gds_service.cc:31-33` 的 `BufferSizeClasses()` 只有 `1M` 和 `16M` 两档,`kBufferMaxPerClass=4`(`gds_service.cc:27`),即池里**最多 4 个 16M buffer**。4M/8M part 都向上取整到 16M 档共享这 4 个;到 16M part + 32 线程并发,4 个 buffer 打满,其余线程走 `allocHostBuffer + registerBuffer`(动态分配+注册 MR)慢路径。setup p50 从 8M 的 41ms 暴涨到 16M 的 196ms,data-plane p50 从 556ms→1166ms。**这是 buffer pool 争抢,非 part_size 本身下界。**

2. **RDMA 16M 回落(6124→3921)**:RDMA 每请求新分配 MR(无共享池),16M 单次 RDMA-read/注册成本变大、并发下 RNIC/NVMe 争用加剧,且单对象仅 4 part、32×4 的调度不如 8 part 均匀。但无 GDS 那种硬池瓶颈,只回落不崩。

> **重要**:GDS 的"16M 崩溃"是 backend buffer pool 配置(2 档×4 个)的人为限制,非 part_size 真实下界。改 `BufferSizeClasses()` 加 8M 档或 `kBufferMaxPerClass` 调大,GDS 16M 很可能不崩甚至更高。故"GDS 最优 8M"严格说是**当前 buffer pool 配置下**最优 8M。

---

## 3. 测试二:conn_pool × num_threads 四组(固定 part_size=8M)

### 3.1 配置

- 固定:`multipart_part_size=8M`(取测试一最优)、client 32 线程、total=64M
- 扫描:`--backend_conn_pool_size` ∈ {8, 16} × `--num_threads` ∈ {8, 16},共 4 组

### 3.2 结果

| 组 | conn_pool | num_threads | GDS throughput | GDS data-plane p50 | GDS wall | RDMA throughput | RDMA data-plane p50 | RDMA wall |
|---|---|---|---|---|---|---|---|---|
| G1 | 8 | 8 | 3198 MiB/s | 543 ms | 6404 ms | 6325 MiB/s | 264 ms | 3238 ms |
| G2 | 8 | 16 | 2987 MiB/s | 612 ms | 6857 ms | **6700 MiB/s** | 254 ms | 3057 ms |
| G3 | 16 | 8 | 2899 MiB/s | 592 ms | 7065 ms | 6307 MiB/s | 265 ms | 3247 ms |
| G4 | 16 | 16 | **3864 MiB/s** | 460 ms | 5300 ms | 6527 MiB/s | 256 ms | 3138 ms |

### 3.3 结论

- **GDS 最优:G4(conn_pool=16 + num_threads=16),3864 MiB/s**。两参数必须**同时**到 16。
- **RDMA 最优:G2(conn_pool=8 + num_threads=16),6700 MiB/s**(但 RDMA 对这俩参数钝感,6300-6700 区间波动 ±6%)。

### 3.4 分析

**GDS 两参数强耦合、单独上调反降**:
- G1(8,8)→ G2(8,16):num_threads 单独翻倍,GDS 降 3198→2987(-7%)。
- G1(8,8)→ G3(16,8):conn_pool 单独翻倍,GDS 降 3198→2899(-9%)。
- G1(8,8)→ G4(16,16):两者同时翻倍,GDS 涨 3198→3864(+21%),data-plane p50 543→460ms。

根因仍是 GDS backend buffer pool 的 4 个 16M buffer:单独加 num_threads 或 conn_pool 都让更多 worker/连接争抢同一组 4 个 buffer,加剧争抢;只有两者同时增多、整体调度能填满 4 个 buffer 并行槽位时才受益。G4 把并发喂饱,反而稳。

**RDMA 对 proxy 侧参数钝感**:G1=6325、G2=6700、G3=6307、G4=6527,波动 ±6%。RDMA 每请求新分配 MR、无 backend 共享 buffer 池,不受 pool 争抢影响;瓶颈在 NVMe/RNIC 硬件带宽,proxy 参数调到 8/16 都够用。最优 num_threads=16(cp=8),但相对 cp=8/nt=8 只快 6%,在误差边缘。

---

## 4. 综合结论

### 4.1 最优配置

| 通路 | 最优 part_size | 最优 conn_pool | 最优 num_threads | 吞吐 |
|---|---|---|---|---|
| GDS | 8M | 16 | 16 | 3864 MiB/s |
| RDMA | 8M | 8 | 16 | 6700 MiB/s |

RDMA 在各自最优配置下约为 GDS 的 1.73×(6700 vs 3864)。

### 4.2 参数影响小结

| 参数 | GDS 影响 | RDMA 影响 |
|---|---|---|
| part_size | 8M 为甜点;16M 受 buffer pool 池大小(4 个)人为限制而崩 | 8M 为甜点;16M 因单次注册成本+调度回落,但仍优于 4M?否,16M(3921)>4M(3567),最优 8M |
| num_threads | 单独上调反降(加剧 buffer 争抢);需与 conn_pool 同步上调 | 钝感,16 略优 |
| conn_pool | 单独上调反降;需与 num_threads 同步上调 | 钝感,8 即可 |

### 4.3 已知瓶颈

1. **GDS backend buffer pool**(`ufile-ac/gds_service.cc`):`BufferSizeClasses={1M,16M}`、`kBufferMaxPerClass=4`。32 线程并发时 4 个 16M buffer 成争抢点,主导 GDS 的参数响应曲线。待验证:改 pool 配置后 GDS 16M 是否不崩、整体是否更高。
2. **RDMA 硬件带宽**:proxy 侧参数调到 8/16 后即达硬件上限,继续上调 proxy 参数收益边际。

### 4.4 数据搬运范围说明(重要)

本轮 backend `--mock-aio-write=1`,**PUT 不落盘**。所测吞吐是 `client GPU/host → proxy → backend RDMA-read` 的搬运段,**不含 NVMe 写**。真实业务(关 mock)的吞吐会低于本报告数值,因为多了落盘开销。本报告适用于"数据搬运路径调参",不直接代表端到端持久化吞吐。

---

## 5. 复现命令

### 5.1 环境(已就绪)

```bash
# ufile-ac(mock 开启)
cd /mnt/us3_test/xinghui.shao/gds/ggds-compile-env/ufile-ac
nohup ./build/ufile-ac \
  --config-file=config/ufile-ac-gds-proxy.ini \
  --mock-aio-write=1 > /tmp/uac_mock.log 2>&1 &

# proxy(按组改 flagfile 后重启)
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
nohup ./build/proxy/us3_turbo_proxy \
  --flagfile=proxy/conf/proxy.flags > /tmp/proxy_bench.log 2>&1 &
```

### 5.2 bench 命令模板

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo

# GDS multipart,part_size/total/concurrency 可调
./build/rtest/bench/gds/us3_turbo_bench_gds_multipart \
  --part-size 8M --total 64M --concurrency 32 --reps 10 --warmup 2

# RDMA multipart
./build/rtest/bench/rdma/us3_turbo_bench_rdma_multipart \
  --part-size 8M --total 64M --concurrency 32 --reps 10 --warmup 2
```

### 5.3 改 proxy 参数流程

```bash
# 编辑 flagfile
sed -i 's/^--num_threads=.*/--num_threads=16/' proxy/conf/proxy.flags
sed -i 's/^--backend_conn_pool_size=.*/--backend_conn_pool_size=16/' proxy/conf/proxy.flags
sed -i 's/^--multipart_part_size=.*/--multipart_part_size=8388608/' proxy/conf/proxy.flags

# 重启 proxy(kill 后单独 nohup 启动,避免 kill 触发脚本中断)
kill $(pgrep -f us3_turbo_proxy); sleep 3
nohup ./build/proxy/us3_turbo_proxy --flagfile=proxy/conf/proxy.flags > /tmp/proxy_bench.log 2>&1 &
```

### 5.4 原始数据日志

每组的完整 bench stdout 已存于 `/tmp/bench_{gds,rdma}_{4M,8M,16M,g1,g2,g3,g4}.log`。

---

## 6. 当前环境状态(测试结束时)

- **ufile-ac**:pid 665905,`--mock-aio-write=1`(mock 开启,纯搬运),监听 192.168.1.198:24000。
- **proxy**:pid 672614,当前 flagfile 为 G4 配置(`num_threads=16, conn_pool=16, multipart_part_size=8M, log_level=warn`),监听 192.168.1.198:9100。
- bench 代码改动(去 4M 上限、透传 part_size、log warn)已编译进 `build/`,未回退。
- proxy flagfile `multipart_part_size` 当前值为 8388608(8M),`num_threads=16`,`backend_conn_pool_size=16`。

---

## 附录:未做但可考虑的后续测试

1. **改 GDS buffer pool 后重测**:调 `BufferSizeClasses()`(加 8M 档)和 `kBufferMaxPerClass`(如 8/16),验证 GDS 16M 是否不崩、GDS 整体吞吐上限是否提升。
2. **关 mock 端到端**:去掉 `--mock-aio-write=1`,测含落盘的真实 PUT 吞吐(预期低于本报告)。
3. **num_threads / conn_pool 更大档位**:32/64,看 GDS 是否继续受益、RDMA 是否到硬件天花板。
4. **client 并发扫描**:client 线程数 16/32/64 对两通路的影响(本轮固定 32)。
