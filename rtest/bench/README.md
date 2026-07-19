# rtest/bench — 性能基准工具

测量 GDS 分段上传的性能，定位瓶颈点。

## 产物

| 可执行 | 通路 | buffer | 链接 |
|--------|------|--------|------|
| `us3_turbo_bench_gds_multipart` | GDS | device 显存 | CUDA (cufile + cudart) |

单源 `multipart_bench.cpp`，编译期宏 `BENCH_GDS` 选通路。

## 用法

```bash
# GDS 串行
us3_turbo_bench_gds_multipart \
  --proxy 192.168.1.198:9100 --total 64M --part-size 16M --reps 5

# GDS 并发
us3_turbo_bench_gds_multipart \
  --total 256M --part-size 16M --concurrency 8 --reps 3 --warmup 1

# CSV 输出
us3_turbo_bench_gds_multipart --total 64M --csv > gds.csv
```

## RDMA 基准

RDMA 单步/分段基准通过 `scripts/run-rdma-bench.sh` 一键运行：

```bash
# 单步并发压测
./scripts/run-rdma-bench.sh --mode single --size 256M --conc 1,2,4,8

# 分段上传
./scripts/run-rdma-bench.sh --mode multipart --total 256M --part-size 16M --conc 8

# 单步 + 分段全部
./scripts/run-rdma-bench.sh --mode both --total 256M --conc 1,2,4,8

# 开启 CRC 校验 + latency trace
./scripts/run-rdma-bench.sh --mode single --verify-crc32c --trace
```

## 参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `--proxy ADDR` | 192.168.1.198:9100 | proxy 控制面 |
| `--total SIZE` | 64M | 对象总大小 |
| `--part-size SIZE` | 16M | part 大小（≤16M，=proxy multipart_part_size）|
| `--reps N` | 5 | 每 worker 重复轮数（串行下即采样数）|
| `--warmup N` | 0 | 预热轮数（不计入，消除 token/descriptor 懒注册与连接池冷启动）|
| `--concurrency N` | 1 | worker 线程数（共享一个 Client）|
| `--bucket` / `--key-prefix` | bench / bench | 命名 |
| `--verify-crc32c` | off | 端到端 CRC32C（GDS 需 D2H，会显著降速，仅定位用）|
| `--trace` | off | client `latency_trace`（token/desc + put 阶段）|
| `--csv` | off | 输出 CSV 而非 summary |

## 测量维度

1. **阶段耗时**：每轮拆 `create`（CreateMultipartUpload）/ `upload`（Σ UploadPart，
   数据面，含 block 级 PutBlock×N）/ `complete`（CompleteMultipartUpload，控制面
   索引写）。`upload` 占 round 的百分比 = 数据面占比。
2. **吞吐**：串行聚合吞吐 + 并发聚合吞吐。
3. **单轮时延分布**：avg/p50/p95/min/max。
4. **CSV**：每行一轮，便于多组对比。
