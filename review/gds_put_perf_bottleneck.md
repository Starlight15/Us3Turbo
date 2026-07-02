# GDS PUT 16M 通路性能瓶颈定位报告

> 测试日期:2026-07-01
> 测试主机:`192.168.1.198`(GPU box `ubuntu`,A800-SXM4-80GB ×8,2TB RAM,128 核)
> 测试对象:Us3Turbo Mode B GDS PUT 通路(client → proxy → backend,backend 反向 RDMA-READ 拉取 client 显存)
> 报告前结论:**当前 5.6 GB/s 的瓶颈不在 PCIe,而在后端软件 CRC32C;关掉 CRC 后真天花板是 100GbE RoCE 网卡线速。**

---

## 1. 测试方法

### 1.1 数据通路

```
client(GPU 显存) ──brpc──> proxy ──brpc──> backend
                                                   │
   client 显存 <── RDMA-READ(RoCE) ── backend(cuObjServer::handlePutObject)
                          │
                   backend pinned host buffer(读后丢弃)
```

- backend 通过 `cuObjServer::handlePutObject` 发起 **DC RDMA-READ**,直接从 client GPU 显存拉数据到 backend 的 pinned host buffer(不写盘,读后丢弃)。
- 数据走 **GPU → NIC 的 P2P**,client 侧没有 D2H bounce。
- 16 MiB 一个 object,单次 PUT 一个 RPC,不做 block 拆分。

### 1.2 关键代码位置

| 角色 | 文件 | 关键点 |
|---|---|---|
| client PUT 主路径 | `client/src/client.cpp:138-188`(`GdsPutOnce`) | AcquireToken → `proxy.GdsPut` → 可选 `VerifyGdsCrc32c` |
| client CRC(D2H + 软件算) | `client/src/client.cpp:54-81`(`VerifyGdsCrc32c`) | `cudaMemcpy` D2H 全量 16MiB 后再软件 CRC |
| client CRC 实现 | `client/src/data/crc32c.cpp:30-37` | 逐字节查表,**无 `_mm_crc32_u64`** |
| backend 数据面 | `backend/src/backend_gds_sink.cpp:103-173`(`ReceiveAndDiscard`) | `handlePutObject` 同步 RDMA-READ + 可选 CRC |
| backend CRC 实现 | `backend/src/common/crc32c.cpp:30-37` | 与 client 同构的逐字节查表,两份独立副本 |
| bench 工具 | `examples/gds/gds_bench_example.cpp` | 多 worker 并发,共享单 Client/单 channel |
| 启动脚本 | `examples/test_gds_bench.sh` | 拉起 backend+proxy+bench,`BACKEND_COMPUTE_CRC32C` 控制后端 CRC |

### 1.3 测试命令

```bash
# 关掉双侧 CRC,看裸 RDMA 通路
BACKEND_COMPUTE_CRC32C=false bash examples/test_gds_bench.sh \
  --size 16M --count N --concurrency C --trace
# client 侧 CRC 由是否带 --verify-crc32c 控制
```

backend 日志每个 PUT 都打印 `rdma_ms` 与 `crc_ms` 两个字段,可直接拆出各段耗时:

```
backend.put object=.../obj-0 length=16777216 transferred=16777216 rdma_ms=3.208 crc_ms=55.361 crc32c=... status=success
```

---

## 2. 测试结果

### 2.1 CRC 关闭(双侧)—— 裸 RDMA 通路

| 并发 | 吞吐 | 单 op `rdma_ms` |
|:---:|:---:|:---:|
| conc=1 | 6.1 GiB/s | 2.55 ms |
| conc=2 | 9.5 GiB/s | ~3.0 ms |
| conc=4 | **10.4 GiB/s**(见顶) | 6.4 ms |
| conc=8 | 9.5 GiB/s(只涨延迟不涨吞吐) | 10.9 ms |

- 并发 ≥4 后吞吐不再上升、只涨延迟 → **网卡被打满**的典型曲线。
- 单流 6.1 GB/s(2.55ms/16MiB),conc=4 时每流降到 ~2.5 GB/s(6.4ms,4 路抢带宽)。

### 2.2 CRC 开启(双侧)—— 之前的 5.6 GB/s

backend 逐 op 日志:

```
rdma_ms=3.208  crc_ms=55.361
```

- 16 MiB 的 RDMA 拉取只要 **3.2 ms**,但软件 CRC32C 要 **55 ms** —— CRC 占了 95% 的时间。
- 这就是"之前 5.6 GB/s"的根因,跟 PCIe / RDMA 都无关。

---

## 3. 瓶颈定位

### 3.1 CRC 开启时:后端软件 CRC32C(CPU bound)

- `backend/src/common/crc32c.cpp:30-37` 与 `client/src/data/crc32c.cpp:30-37` 都是 **Castagnoli 反射多项式 + 256 项查表 + 逐字节循环**,没有用 SSE4.2 的 `_mm_crc32_u64` 硬件指令。
- 实测算力 ~290 MB/s/核,16 MiB 要 ~55 ms。
- backend CRC 在 `backend_gds_sink.cpp:156-164` 的 **同步 RPC handler 路径上**,RDMA 完成后立刻算,算完才回包。所以每个 PUT 的端到端时间 ≈ `rdma_ms + crc_ms`。
- client 侧若开 `--verify-crc32c`,还会再做一次 D2H 全量拷贝 + 软件 CRC(`client.cpp:54-81`),雪上加霜。

### 3.2 CRC 关闭后:100GbE RoCE 网卡线速,不是 PCIe

**网卡侧:**
- 承载数据的网卡是 `ens13f0np0` = `mlx5_2`(Mellanox MT4125,PCI `0000:7d:00.0`),**100 GbE RoCE**,`LnkSta: 16GT/s Width x16`。
- 100 GbE 理论线速 = 11921 MiB/s;conc=4 实测 10.4 GiB/s ≈ **线速的 89%** → 网卡打满。

**PCIe 侧(确认没打满):**
- GPU0 PCIe = **Gen4 x16**(`nvidia-smi -i 0 -q`:Current=Gen4 / 16x;`lspci`:`LnkSta Speed 16GT/s Width x16`)。
- 纯 D2H 带宽对照实验(`cudaMemcpy` D2H 4×256MiB)= **21.7 GB/s**。
- RDMA 聚合仅占 PCIe ~48%(10.4 / 21.7),还有 2 倍余量。
- 数据走 GPU→NIC P2P,client 侧无 D2H bounce,故 PCIe 利用率本来就低。
- 反证:若 PCIe 受限,64 MiB(4×16)只要 ~2.9ms;conc=4 实测 6.4ms,**说明瓶颈在网线而非 PCIe**。

### 3.3 GPU↔NIC 拓扑

`nvidia-smi topo -m`:GPU0 ↔ NIC2(`mlx5_2`)= **PXB**(同 PCIe switch),无跨 NUMA。拓扑不是瓶颈。

---

## 4. 结论

| 场景 | 瓶颈 | 实测 | 天花板 |
|---|---|---|---|
| CRC 开(原状态) | 后端软件 CRC32C(CPU) | ~5.6 GB/s | ~290 MB/s/核 × 核数 |
| CRC 关 | 100GbE RoCE 网卡 | 10.4 GiB/s | 11.6 GiB/s(线速 89%) |
| PCIe Gen4 x16(对照) | — | D2H 21.7 GB/s | 未打满,2× 余量 |

**当前 5.6 GB/s 不是 PCIe 打满,而是后端软件 CRC32C 把 CPU 打满了。** 关掉 CRC 后,真天花板是 100GbE 网卡(10.4 GiB/s),PCIe 仍有 2 倍余量。

---

## 5. 优化方向

### 5.1 想保留 CRC 又要吞吐:换硬件 CRC32C(推荐)

把 `backend/src/common/crc32c.cpp` 与 `client/src/data/crc32c.cpp` 的逐字节查表循环换成 SSE4.2 `_mm_crc32_u64`(CRC32C 硬件指令):

- 预期算力 ~50+ GB/s,16 MiB 的 CRC 从 55ms 降到 ~0.3ms。
- CRC 降为非瓶颈后,端到端 ≈ 纯 RDMA 时间,吞吐可逼近 100GbE 线速。
- **两份副本必须算法一致**(项目刻意两份独立同构,改要同步改)。
- `backend/src/common/crc32c.h` 已注释标注这是 V3 计划。

### 5.2 裸吞吐:CRC 关着

10.4 GiB/s 已是这块 100GbE 网卡的极限。要更高:
- 换 200/400 GbE NIC;或
- 多 NIC 并行(当前只用了 `mlx5_2` 一块,机器上还有 `mlx5_0/1/4/5` 等可用口)。

### 5.3 不建议的方向

- 不必动 PCIe / GPU 拓扑:余量充足。
- 不必拆 16M block 成更小并发:conc≥4 已网卡打满,拆了也只是涨延迟。
