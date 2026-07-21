# rtest/bench — 性能基准工具

GDS 和 RDMA 两条通路的性能基准，测量吞吐与时延分布。

## 产物

| 可执行 | 通路 | buffer | 链接 | 模式 |
|--------|------|--------|------|------|
| `us3_turbo_bench_gds_put` | GDS | device 显存 | CUDA (cufile + cudart) | 单步 PUT |
| `us3_turbo_bench_gds_multipart` | GDS | device 显存 | CUDA (cufile + cudart) | 分段上传 |
| `us3_turbo_bench_rdma_put` | RDMA | host 内存 | ibverbs + rdmacm | 单步 PUT |
| `us3_turbo_bench_rdma_multipart` | RDMA | host 内存 | ibverbs + rdmacm | 分段上传 |

共享基础设施：`harness.h`（BaseArgs / RoundResult / ComputeStats / PrintReport / WriteCsv）。

## 用法

### GDS 单步 PUT

```bash
us3_turbo_bench_gds_put \
  --proxy 192.168.1.198:9100 --size 100M --count 100 --concurrency 8
```

### GDS 分段上传

```bash
# 串行
us3_turbo_bench_gds_multipart \
  --proxy 192.168.1.198:9100 --total 64M --part-size 4M --reps 5

# 并发
us3_turbo_bench_gds_multipart \
  --total 256M --part-size 4M --concurrency 8 --reps 3 --warmup 1

# CSV 输出
us3_turbo_bench_gds_multipart --total 64M --csv > gds_mp.csv
```

### RDMA 单步 PUT

```bash
us3_turbo_bench_rdma_put \
  --proxy 192.168.1.198:9100 --size 100M --count 100 --concurrency 8
```

### RDMA 分段上传

```bash
us3_turbo_bench_rdma_multipart \
  --proxy 192.168.1.198:9100 --total 64M --part-size 4M --reps 5

# CSV 输出
us3_turbo_bench_rdma_multipart --total 64M --csv > rdma_mp.csv
```

### 一键脚本

```bash
# RDMA 单步 + 分段
./scripts/run-rdma-bench.sh --mode both --total 256M --conc 1,2,4,8

# GDS 单步 + 分段
./scripts/run-gds-bench.sh --total 256M --conc 1,2,4,8
```

## 参数

| 参数 | 默认 | 适用 | 说明 |
|------|------|------|------|
| `--proxy ADDR` | 192.168.1.198:9100 | 全部 | proxy 控制面 |
| `--total SIZE` | 64M | multipart | 对象总大小 |
| `--size SIZE` | 100M | put | 单个对象大小 |
| `--count N` | 10 | put | 总对象数 |
| `--part-size SIZE` | 4M | multipart | part 大小（≤16M，=proxy multipart_part_size）|
| `--reps N` | 5 | multipart | 每 worker 重复轮数 |
| `--warmup N` | 0 | 全部 | 预热轮数（不计入统计，消除 token 懒注册与连接池冷启动）|
| `--concurrency N` | 1 | 全部 | worker 线程数（共享一个 Client）|
| `--bucket` / `--key-prefix` | bench | 全部 | 命名 |
| `--verify-crc32c` | off | 全部 | 端到端 CRC32C（GDS 需 D2H，会显著降速）|
| `--trace` | off | 全部 | client latency_trace |
| `--csv` | off | multipart | 输出 CSV 而非 summary |

## 测量维度

### 单步 PUT bench
1. **吞吐**：多 worker 并发聚合吞吐（MiB/s, ops/s）
2. **时延分布**：per-PUT latency（min/p50/p95/p99/max/avg）

### 分段上传 bench
1. **阶段耗时**：每轮拆 setup（CreateMultipartUpload）/ data-plane（ΣUploadPart）/ control-plane（CompleteMultipartUpload），data-plane 占比 = 数据传输效率
2. **吞吐**：串行聚合 + 并发聚合
3. **时延分布**：avg/p50/p95/min/max per-round
4. **CSV**：每行一轮，便于多组对比

## 架构

```
bench/
├── harness.h              # 共享基础设施（通路无关，header-only）
├── gds/
│   ├── gds_put_bench.cpp
│   └── gds_multipart_bench.cpp
└── rdma/
    ├── rdma_put_bench.cpp
    └── rdma_multipart_bench.cpp
```

每个 bench 文件遵循统一结构：
1. `Args` 派生自 `rtest::bench::BaseArgs`，添加通路特定字段
2. `RunOneRound()` 返回 `RoundResult`（multipart）或 `do_put` lambda（put）
3. `Worker()` — 通路特定（GDS: cudaMalloc + H2D，RDMA: host buffer）
4. `main()` — ParseArgs → 初始化 Client → 跑 workers → 聚合 → PrintReport
