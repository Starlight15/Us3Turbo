# Us3Turbo 分段上传性能基准报告（GDS vs UCX）

> 环境：192.168.1.198，proxy(:9100) → ufile-ac(:24000, GDS RDMA :18666 / UCX mlx5_2:1) + dbgate(:20165) + mongod(:27017)
> 日期：2026-07-14（UTC）。测试前重启全部模块，每模块单进程。
> 工具：`rtest/bench/us3_turbo_bench_{gds,ucx}_multipart`（本目录 `multipart_bench.cpp`）。
> UCX 用例需 `UCX_NET_DEVICES=mlx5_2:1`（否则默认 mlx5_0 跨网段超时，见 TEST_FINDINGS.md P2）。

## A. 关键对照：单步 PUT vs 分段上传（TCP_NODELAY 修复后）

> 客户端 CRC 全程关闭（`--verify-crc32c` 未传，默认 false）。后端 ufile-ac 的 CRC 用硬件 SSE4.2
> `_mm_crc32`（`g_crc32c_type=CRC32C_INTEL`，`ac_server.cc:51`），**不是瓶颈**。

**2026-07-15 两项修复**：(1) proxy multipart 改 **1 block/part**（每 part 一次 `PutBlockGds/Ucx` 写整 16M，
不再 4×4M 串行）；(2) 后端 `InternalEntry::length_` 加宽 24→32 位支持满 16M 落盘+读回；
**(3) proxy `TcpConnection::Connect` 加 `TCP_NODELAY`**（消 Nagle/delayed-ACK 40ms/RPC）。
10 个回归用例（GDS+UCX）全 PASS。下方为 TCP_NODELAY 后数据。

单步 PUT（`us3_turbo_gds_bench_example`，整对象 = 1 个 `PutBlockGds`，≤16M，crc off）：

| 并发 | 单步吞吐 | 单步单轮 p50 |
|------|----------|--------------|
| conc=4  | 804 MiB/s  | 118 ms |
| conc=8  | 1229 MiB/s | 128 ms |
| **conc=48** | **3120 MiB/s ≈ 3.0 GiB/s** | 387 ms |

分段上传改 + NODELAY 后（`multipart_bench`，128M / 8 part / 每 part 1 个 16M block）：

| 并发 | 分段吞吐 | 分段单轮 total p50 |
|------|----------|--------------|
| conc=1  | 494 MiB/s  | 241 ms |
| conc=4  | 1065 MiB/s | 475 ms |
| conc=8  | **1047 MiB/s** | 981 ms |
| conc=16 | 993 MiB/s（拐点） | 1983 ms |
| conc=32 | 1137 MiB/s | 3376 ms |
| conc=48 | 1065 MiB/s | 5515 ms |

**TCP_NODELAY 是本次最大收益**：proxy↔dbgate 与 proxy↔ufile-ac 的请求都被拆成 `[4B头][body]` 两次 send，
无 NODELAY 时 body 小包被 Nagle 卡等头部 ACK（对端 delayed-ACK 默认 40ms）。Complete 5 次 RPC × 40ms = 200ms
→ 修复后 **complete 210ms→4ms**（-51×），单 part **upload 207ms→32ms**（-6.5×），单流吞吐 **35→385 MiB/s**（11×）。

**新瓶颈定位**（TCP_NODELAY 后）：
1. **数据面单流 ~32ms/16M（GDS）**：RDMA 读 + 硬件 CRC + memcpy + aio_write，单流 ~500 MiB/s。
2. **GDS 并发拐点 conc≈4-8（~1.05 GiB/s）**：8 之后不再升，单轮 p50 随并发线性涨（排队）。
   瓶颈 = proxy `num_threads=4`(brpc) + backend `worker_threads=8` + 单 NVMe 落盘。
3. **UCX 比 GDS 慢 ~2×**：单流 283 vs 494 MiB/s；ufile-ac 反向 `ucp_get_nbx` 远程读 host 内存多一次 RMA 拉取。

**"GDS 6-10G 上限"指单步 PUT 路径（旧 cuObjServer）；当前 ufile-ac 单步峰值 ~3 GiB/s。**

---

- **数据面（UploadPart）随对象增大主导耗时**：64M 时已占单轮 76%，128M 占 86%。串行吞吐随 total 上升逼近数据面天花板。
- **控制面（Create+Complete）固定开销大**：单轮 **Complete ≈ 210ms**（与对象大小无关），由 4 次 dbgate→MongoDB 往返（Get minit / ListParts / InsertFileIdx upsert / Remove）串行构成，是**小对象吞吐的头号瓶颈**。
- **GDS vs UCX 数据面基本持平**：单 part 16M 上传 GDS ≈ 187ms、UCX ≈ 206ms；ufile-ac 侧单 block(4M) `gds_put` p50=5ms / `ucx_put` p50=10ms。UCX 略慢（ufile-ac 反向 `ucp_get_nbx` 远程读 host 内存，比 GDS 直接 RDMA 写多一次 RMA 拉取）。
- **并发线性扩展到 ~4，8 之后趋平**：128M conc 1→4 几乎线性（68→240 MiB/s），conc 8 达 320 MiB/s，conc 16 不再上升（322 MiB/s）——proxy `num_threads=4` + backend `worker_threads=8` + 单 NIC 成为上限。
- **CRC32C 校验开销显著**：GDS 开 `verify_crc32c` 后单 part 上传 187→1027ms（+5.5×），因 client 侧软件 CRC32C 需 D2H 后逐字节算（~0.9GB/s），与历史结论一致。

## 1. 测试方法

`multipart_bench` 每轮完整执行 `CreateMultipartUpload → UploadPart×N → CompleteMultipartUpload`，分别计时三个阶段：

- `create`：CreateMultipartUpload（1 次 dbgate InsertMinit）。
- `upload`：Σ UploadPart（数据面，每 part 内 proxy 串行发 block_count 个 PutBlock 到 ufile-ac）。
- `complete`：CompleteMultipartUpload（Get minit + ListParts + InsertFileIdx upsert + Remove，4 次 dbgate 往返）。

参数：`--total`（对象总大小）、`--part-size`（默认 16M=proxy 上限）、`--reps`、`--warmup`、`--concurrency`。
part_size 固定 16M（proxy 约束：非 last part 必须 ==16M；更小 part 会被 Complete 以 `invalid part size` 拒绝，见下"已知限制"）。

## 2. 串行吞吐（conc=1, reps=5, part=16M，TCP_NODELAY 后）

| total | parts | GDS tput(MiB/s) | GDS upload(ms) | GDS complete(ms) | UCX tput(MiB/s) | UCX upload(ms) | UCX complete(ms) |
|-------|-------|-----------------|----------------|------------------|-----------------|----------------|------------------|
| 16M   | 1     | 385            | 32             | 4.1              | 283            | 56             | 7.0              |
| 128M  | 8     | 494            | 252            | 4.8              | 283            | 444            | 7.0              |

观察：
- `complete` 从改前 210ms → **~4ms**（TCP_NODELAY 消掉 Nagle 40ms/RPC），不再是瓶颈。
- `upload` 单流 GDS 32ms/16M ≈ 500 MiB/s；UCX 56ms/16M ≈ 285 MiB/s。
- 单 part 16M：GDS total 37ms（吞吐 385 MiB/s），UCX total 63ms。

## 3. 并发吞吐（total=128M, part=16M，TCP_NODELAY 后）

| conc | GDS tput(MiB/s) | GDS total p50(ms) | GDS complete(ms) | UCX tput(MiB/s) | UCX total p50(ms) |
|------|-----------------|--------------------|------------------|-----------------|--------------------|
| 1    | 494            | 241               | 4.8              | 283            | 418               |
| 4    | 1065           | 475               | 6.7              | 477            | 1051              |
| 8    | 1047           | 981               | 9.0              | 551            | 1711              |
| 16   | 993            | 1983              | 51(p95 162)      | 581            | 3529              |
| 32   | 1137           | 3376              | 129(p95 371)     | —              | —                 |
| 48   | 1065           | 5515              | 243(p95 721)     | —              | —                 |

观察：
- GDS conc 1→4 近线性（494→1065，2.2×）；conc≥8 吞吐见顶 ~1.05 GiB/s，单轮 p50 随并发线性涨 → 纯排队，不再提升吞吐。
- **GDS 拐点 conc≈4-8，吞吐天花板 ~1.05 GiB/s**（~9% of 100GbE）。瓶颈：proxy `num_threads=4` + backend `worker_threads=8` + 单 NVMe 落盘。
- `complete` 在 conc≤8 仍 ~5-9ms；conc≥16 排队升到 50-243ms → dbgate 池(pool=4)+MongoDB 排队，但已非主要瓶颈。
- UCX conc=16 仍微升到 581，天花板 ~580 MiB/s（GDS 的 ~55%）。

## 4. 单 part 延迟对比（conc=1, 16M, 1 part, reps=8, warmup=2）

| 阶段 | GDS (ms) | UCX (ms) |
|------|----------|----------|
| create | 42 | 42 |
| upload | 187 | 206 |
| complete | 210 | 210 |
| total | 439 | 458 |

- GDS upload 187ms / 16M = **85 MiB/s** 单流；UCX 206ms / 16M = **78 MiB/s**。
- ufile-ac 侧单 block(4M)：`gds_put` p50=5ms（→ ~800MiB/s 后端内部），`ucx_put` p50=10ms（→ ~400MiB/s）。
  即**后端单 block 处理很快，4 block 串行 = 20~40ms，但端到端单 part 187~206ms** → 差额（~150ms）在 proxy↔ufile-ac TCP 往返 + 串行 block 之间的等待 + client↔proxy brpc 往返。

## 5. CRC32C 开销（GDS, conc=1, 64M, reps=3）

| 模式 | tput(MiB/s) | upload(ms) |
|------|-------------|------------|
| crc off | 60.0 | 814 |
| crc on  | 50.0 | 1027 |

开 `verify_crc32c` 后 client 对每 part D2H + 软件 CRC32C（`Crc32c` 逐字节，~0.9GB/s），单 part 上传 +27%，吞吐 -17%。与 [[gds-put-perf-bottleneck]] 记录的 CPU-bound 软件 CRC 一致。

## 6. 瓶颈定位（TCP_NODELAY 修复后）

### 瓶颈 1（已修复）：控制面 Complete 210ms → 4ms

**根因**：proxy↔dbgate 请求拆成 `[4B长度头][body]` 两次 send（`dbgate_client.cpp:73-78`），proxy 侧
`TcpConnection::Connect` 无 `TCP_NODELAY` → body 小包被 Nagle 卡等头部 ACK，叠加 dbgate delayed-ACK
默认 40ms，**每条 RPC 固定 ~40ms**。Complete 5 次串行 RPC（Get/ListParts/InsertFileIdx/DeleteParts/DeleteMinit）
× 40ms ≈ 200ms。叠加 dbgate 空闲 3min 关连接（`umongo-gateway main.go:167 clientReadTimeout=3min`）使
proxy 池连接变 CLOSE-WAIT、首笔 RPC 失败重建，进一步放大。

**修复**：`tcp_connection.cpp:Connect` 加 `setsockopt(IPPROTO_TCP, TCP_NODELAY)`。
**效果**：complete 210ms→4ms（-51×）；连带 upload 207ms→32ms（proxy↔ufile-ac 控制包同踩 Nagle）；
单流吞吐 35→385 MiB/s（11×）。

### 瓶颈 2：GDS 并发天花板 ~1.05 GiB/s @ conc≈4-8（worker/num_threads 提升验证，2026-07-15）

TCP_NODELAY 后单流 GDS 32ms/16M ≈ 500 MiB/s，但 conc≥8 吞吐见顶 ~1.05 GiB/s（单轮 p50 随并发线性涨=排队）。
**按"提升路径"依次实测 backend `worker_threads` 与 proxy `num_threads`，结论：两者提升均无效，天花板是软件框架瓶颈，非线程数。**

测试矩阵（GDS multipart 256M / part=16M，每点 iostat 校验 NVMe 实写）：

| 配置 | conc8 | conc16 | conc24 | conc32 |
|------|-------|--------|--------|--------|
| 基线 worker=8 proxy=4 | 1062 | — | — | — |
| worker=16 proxy=4 | 1105 | 1099 | 1180 | 1224 |
| worker=32 proxy=4 | 1107 | 1199 | 1191 | 1203 |
| worker=32 proxy=16 | 1037 | 1082 | 1080 | 1135 |

UCX 256M（worker=32 + proxy=16）：conc8=548 / conc16=640 / conc24=637 / conc32=612 MiB/s。

**per-op 计时对比（worker=8 vs worker=32，相同负载）**：
- `worker_queue_wait` p50：**83ms → 27us**（worker 翻倍后排队彻底消失 → worker 不是瓶颈）
- `rdma_read` p50：11ms → 25ms（负载下变慢）
- `aio_write` p50：31ms → 77ms
- 单 op `worker_wait_total` p50：101ms → 192ms

**根因（软件框架瓶颈，代码已确认）**：`ufile-ac/main.cc:124` 只创建 `EventWorker(0, ...)` ——
全 backend 只有 **1 个 EventLoop 线程**处理所有落盘。`gds worker_threads=32` 只并行了 RDMA-read+CRC
（`gds_service.cc:WorkerLoop` → `HandlePut`），之后每个 op 都 `loop_->QueueInLoop(owner_->PutFromGds)`
（`gds_service.cc:273`）排到**同一个 EventLoop 线程**串行执行 `memcpy(16M)`+`EncodeDeviceEntry`+
`AllocWriteOffset`+`SubmitWrite`（`ac_server.cc:PutFromGds` → `ioContext.cc:WriteDevice`）。这个单线程是闸门。
`gds_service.cc:WorkerLoop` 的 worker 在 `ctx->cv.wait` 等 EventLoop 完成（`worker_wait_total`），单 loop 喂盘速率决定全局吞吐。

**硬件天花板 vs 实测（iostat nvme3n1）**：
- fio 裸 NVMe 顺序写（libaio, bs=1M, iodepth=32, direct）：**3727 MiB/s ≈ 3.64 GiB/s**
- 框架实际喂盘：峰值 ~1200 MB/s，**NVMe %util 仅 28%**
- → **NVMe 还有 ~3× 余量未用，瓶颈在 ufile-ac 单 EventLoop 串行 memcpy+aio_submit，不是硬件**

**优化方向（需改 ufile-ac 框架代码，未实施）**：
1. 多 EventLoop（per-NVMe 或 per-worker 一个 loop）替代单 `EventWorker(0,...)`，让 memcpy+SubmitWrite 并行。
2. 把 `memcpy`+`EncodeDeviceEntry`+`AllocWriteOffset` 移出 EventLoop（放回 gds worker 线程并行），EventLoop 只剩 `SubmitWrite`（异步，本就不阻塞）。
3. UCX 改 RDMA write 方向（client→backend）替代反向 `ucp_get_nbx` 拉取，逼近 GDS。

### 瓶颈 3：UCX 比 GDS 慢 ~2×（反向 RMA 拉取）

UCX 单流 283 vs GDS 494 MiB/s。ufile-ac 反向 `ucp_get_nbx` 从 client host 内存远程读，比 GDS 直接 RDMA 写
多一次 RMA 拉取往返。

### 测量注意：单步 bench warmup 模式的吞吐假象

`rtest/examples/us3_turbo_gds_bench_example` 在 `--warmup` 较大时报出 9000–17000 MiB/s，**不可信**：
warmup 与正式阶段用同一 `key_prefix`，重复 key 命中 `PutFromGds` 的 `keysmap_->Get!=NULL` 分支
（`ac_server.cc:1342` key duplicate → `ReadDataHeader` 提前返回，**不落盘**）。warmup 越多、key 复用越多，
落盘越少 → 报出吞吐虚高。**以 iostat nvme3n1 实写为准 ≈ 1.1 GiB/s**；fresh unique key（warmup=0 或
每次换 `--key-prefix`）下单步 PUT 也仅 ~1.1 GiB/s，与 multipart 一致。

## 7. 已知限制

- **part_size 只能 16M**：proxy `ValidatePartSizes` 要求非 last part == `multipart_part_size`(16M)。`--part-size 8M/4M` 会触发 Complete 报 `invalid part size`（bench 计为 fail）。这是 proxy 语义，非 bench 缺陷。测不同 block 粒度需改 proxy flag。
- **ufile-ac RDMA :18666 未单独 listen**：GDS RDMA 端口由 backend 按需建立，`ss` 看不到 18666 LISTEN 属正常（连接态）。
- **trace 模式**：`--trace` 开启 client `latency_trace`，但 multipart 的 UploadPart 走 `Client::UploadPartGds/Ucx`（`client.cpp`）而非 `PutChannel`，故 `token/desc + put` 阶段 trace 不打印（trace 仅对单步 `PutObject` 生效）。阶段计时以 bench 自身的 create/upload/complete 为准。

## 8. 复现

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo/build
# 串行扫 total
for t in 16M 32M 64M 128M; do
  ./rtest/bench/us3_turbo_bench_gds_multipart --total $t --part-size 16M --reps 5 --warmup 1
  UCX_NET_DEVICES=mlx5_2:1 ./rtest/bench/us3_turbo_bench_ucx_multipart --total $t --part-size 16M --reps 5 --warmup 1
done
# 并发扫
for c in 1 2 4 8 16; do
  ./rtest/bench/us3_turbo_bench_gds_multipart --total 128M --part-size 16M --reps 3 --warmup 1 --concurrency $c
done
# CSV
./rtest/bench/us3_turbo_bench_gds_multipart --total 64M --csv > gds.csv
```

## 9. 数据文件

- bench 二进制：`build/rtest/bench/us3_turbo_bench_{gds,ucx}_multipart`
- proxy 日志：`/tmp/proxy_bench.log`
- ufile-ac 日志：`/mnt/us3_test/ld/log/set1/set01-m00-d00/ufile-ac.*.log`（含 `gds_put`/`ucx_put` per-op 延迟表）
- dbgate 日志：`/mnt/us3_test/xinghui.shao/gds/dbgate/log/stat-*.log`（含 OP_INSERT/UPDATE/QUERY/DELETE 延迟）

## 10. 变更记录

- **2026-07-15（1）**：proxy multipart 改 **1 block/part**（删 4×4M 串行切分，`multipart.cpp:209/269` 每整 part
  单次 `PutBlockGds/Ucx`）；后端 `InternalEntry::length_` 加宽 24→32 位（15→16 字节）以支持满 16M 落盘+读回
  （`ufile-ac/keymaps.h:36`，两份副本 ld/ufile-ac 与 ggds-compile-env/ufile-ac 同步）。10 回归全 PASS。
- **2026-07-15（2）TCP_NODELAY**：proxy `TcpConnection::Connect` 加 `setsockopt(IPPROTO_TCP, TCP_NODELAY)`
  （`tcp_connection.cpp`）。根因：proxy↔dbgate / proxy↔ufile-ac 请求拆 `[4B头][body]` 两次 send，无 NODELAY 触发
  Nagle/delayed-ACK 40ms/RPC；Complete 5 次串行 RPC × 40ms ≈ 200ms。修复后 complete 210ms→4ms、单流
  35→385 MiB/s、GDS 并发天花板 ~1.05 GiB/s@conc8。10 回归全 PASS。
- **2026-07-15（3）提升路径验证（worker_threads + num_threads）**：按"提升瓶颈"路径依次实测
  backend `worker_threads` 8→16→32（`ufile-ac/config/ufile-ac-gds-proxy.ini` `[gds]/[ucx]`，两份副本同步）、
  proxy `--num_threads` 4→16 + `--backend_conn_pool_size`/`--dbgate_conn_pool_size` 8→16。结论：**两者提升对吞吐均无效**，
  GDS 仍卡 ~1.1 GiB/s、UCX ~0.6 GiB/s。代码+日志定位真正瓶颈 = ufile-ac 单 EventLoop 线程（`main.cc:124 EventWorker(0,...)`）
  串行 memcpy+aio_submit；fio 裸 NVMe 3727 MiB/s、实测喂盘仅 ~1200MB/s（util 28%）→ 软件框架瓶颈非硬件。
  测试后 backend/proxy 配置**保持 worker=32 / num_threads=16**（无害，worker_queue_wait 已清零）。**未改任何代码。**
  关键证据：`worker_queue_wait` p50 83ms→27us（worker 不是瓶颈），`worker_wait_total` p50 101ms→192ms。
