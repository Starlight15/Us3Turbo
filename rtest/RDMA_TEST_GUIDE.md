# RDMA (libibverbs) 测试指南

> 测试环境：192.168.1.198，proxy(:9100) → ufile-ac(:24000, RDMA CM :18666) + dbgate(:20165) + mongod(:27017)

本文档说明 `scripts/` 下 RDMA 回归测试与性能基准脚本的使用方法。

---

## 环境准备

### 1. 启动后端服务

```bash
# proxy（必须指向真实 ufile-ac 后端，非已废弃的 us3_turbo_backend）
./build/proxy/us3_turbo_proxy \
  --bind_host=192.168.1.198 --proxy_port=9100 \
  --backend_endpoint=192.168.1.198:24000 \
  --dbgate_endpoint=127.0.0.1:20165
```

### 2. 编译

脚本首次运行会自动编译缺失 target；也可手动预编译：

```bash
cmake --build build -j$(nproc)
```

---

## 回归测试: `run-rdma-regression.sh`

一次性运行全部 RDMA 回归测试，对应 `rtest/regression/rdma/` 下 3 个独立可执行。

### 测试用例

| 测试 | 可执行 | 说明 |
|------|--------|------|
| T1.1 | `us3_turbo_rtest_rdma_multipart_invalid_part_size` | 3 个 < part_size 的 part → Complete 应被 `ValidatePartSizes` 拒绝，error 含 "invalid part size" |
| T1.2 | `us3_turbo_rtest_rdma_multipart_part_number_violation` | 场景A 重复 part_number(1,1,2) + 场景B 跳号(1,3) → 任一非静默失败即 PASS |
| T1.3 | `us3_turbo_rtest_rdma_multipart_single_part` | 单 part = 整对象 → Complete 成功 + `object_size == part_size` |

### 用法

```bash
# 默认参数（proxy=192.168.1.198:9100, part-size=4M）
./scripts/run-rdma-regression.sh

# 自定义 proxy
./scripts/run-rdma-regression.sh --proxy 10.0.0.1:9100

# 自定义 part 大小
./scripts/run-rdma-regression.sh --part-size 8M

# 开启 CRC32C 校验（仅 T1.3 生效）
./scripts/run-rdma-regression.sh --verify-crc32c
```

### 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--proxy ADDR` | `192.168.1.198:9100` | proxy 控制面地址 |
| `--part-size SIZE` | `4M` | part 大小（≤16M） |
| `--verify-crc32c` | off | 开启端到端 CRC32C（仅 T1.3 单 part 校验） |

### 输出示例

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[test] T1.1 invalid_part_size
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  CreateMultipartUpload: upload_id=<uuid>
  UploadPartRdma 1 ok etag=<etag>
  UploadPartRdma 2 ok etag=<etag>
  UploadPartRdma 3 ok etag=<etag>
  CompleteMultipartUpload: FAILED (expected) error=invalid part size: ...
[PASS] rdma_multipart_invalid_part_size

════════════════════════════════════════════════════════
  total : 3
  pass  : 3
  fail  : 0
  skip  : 0
════════════════════════════════════════════════════════
```

退出码：
- `0` — 全部 PASS
- `1` — 存在 FAIL
- T1.2 场景B 环境不稳时可能输出 `SKIP`（exit=77），不计 FAIL

---

## 性能基准: `run-rdma-bench.sh`

基于 `us3_turbo_rdma_put_example`，支持单步（`--mode single`）和分段（`--mode multipart`）两种模式，可指定并发度列表。

### 单步 PUT 模式

每 worker 分配独立 host buffer，通过 `PutObject(kRdma)` 单次上传。适合测试单步吞吐与并发缩放。

```bash
# 串行（100M 对象，1 次）
./scripts/run-rdma-bench.sh --mode single --size 100M

# 并发梯度
./scripts/run-rdma-bench.sh --mode single --size 256M --conc 1,2,4,8,16

# 大对象 + CRC 校验
./scripts/run-rdma-bench.sh --mode single --size 16M --verify-crc32c

# 开启 latency_trace（诊断阶段耗时）
./scripts/run-rdma-bench.sh --mode single --trace --reps 1
```

### 分段 PUT 模式

通过 `CreateMultipartUpload → UploadPartRdma ×N → CompleteMultipartUpload` 完成。

```bash
# 串行分段（默认 256M / 4M-part = 64 parts）
./scripts/run-rdma-bench.sh --mode multipart

# 大 part（16M）+ 并发
./scripts/run-rdma-bench.sh --mode multipart --total 256M --part-size 16M --conc 8

# 分段 CRC 校验
./scripts/run-rdma-bench.sh --mode multipart --total 64M --part-size 4M --verify-crc32c
```

### 单步 + 分段组合

```bash
# 同时跑单步和分段，各并发度
./scripts/run-rdma-bench.sh --mode both --total 256M --size 256M --conc 1,2,4,8
```

### 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--mode MODE` | `single` | `single` \| `multipart` \| `both` |
| `--proxy ADDR` | `192.168.1.198:9100` | proxy 控制面地址 |
| `--conc 1,2,4,8` | `1` | 逗号分隔的并发列表 |
| `--verify-crc32c` | off | 端到端 CRC32C 校验 |
| `--trace` | off | client `latency_trace`（拆 acquire + rpc 阶段） |

**单步专用：**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--size SIZE` | `100M` | 对象大小 |
| `--reps N` | `5` | 每 worker 重复次数 |

**分段专用：**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--total SIZE` | `256M` | 对象总大小 |
| `--part-size SIZE` | `4M` | part 大小（≤16M） |
| `--mp-reps N` | `3` | 每 worker 重复轮数 |
| `--warmup N` | `1` | 预热轮数（不计入统计） |

### 输出示例

```
════════════════════════════════════════════════════════
  RDMA Bench
  proxy    : 192.168.1.198:9100
  mode     : both
  conc     : 1,2,4,8
════════════════════════════════════════════════════════

--- 单步 PUT: size=256M conc=1 reps=5 ---
concurrency=1 reps=5 size=268435456 ok=5 fail=0 wall_sec=0.987 throughput_MiBps=259.5

--- 分段 PUT: total=256M part-size=4M conc=1 reps=3 warmup=1 ---
multipart ok object_id=<id> etag=<etag> size=268435456 parts=64 wall_sec=2.134 throughput_MiBps=120.0

... (conc=2,4,8 类似) ...

════════════════════════════════════════════════════════
  全部完成，总耗时: 45s
════════════════════════════════════════════════════════
```

---

## 回归测试独立运行

也可以直接调用编译产物，不通过脚本（用于 CI 或精细控制）：

```bash
# T1.1
./build/rtest/regression/rdma/us3_turbo_rtest_rdma_multipart_invalid_part_size \
  --proxy 192.168.1.198:9100 --part-size 3M

# T1.2
./build/rtest/regression/rdma/us3_turbo_rtest_rdma_multipart_part_number_violation \
  --proxy 192.168.1.198:9100

# T1.3
./build/rtest/regression/rdma/us3_turbo_rtest_rdma_multipart_single_part \
  --proxy 192.168.1.198:9100 --verify-crc32c
```

---

## 注意事项

1. **proxy 空闲超时**：proxy 与 dbgate 的连接空闲 3~5 分钟后可能变 CLOSE-WAIT，导致 `CreateMultipartUpload` 失败。跑测试前若 proxy 已空闲数分钟，先重启 proxy 或发一个轻量请求预热。
2. **并发 Complete 竞态**：`WritePartIndex` 的 `UpdateMergedSize` 非原子，conc≥4 时 `CompleteMultipartUpload` 可能因 `merged_size != parts_sum` 失败。这是已知的共享代码问题，非 RDMA 特有。建议 conc≤2。
3. **CRC32C 开销**：RDMA 路径 CRC32C 在 host buffer 上直算（无 D2H），开销极小，可在回归测试中保持开启。
4. **part_size 上限**：proxy 限制 `FLAGS_multipart_part_size`（默认 16MB），即 `--part-size` 须 ≤16M。非 last part 必须恰好等于此值。
