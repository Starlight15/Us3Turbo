# rtest/ — Us3Turbo 测试体系

端到端示例 + 性能基准 + 正确性回归测试。

## 目录结构

```
rtest/
├── README.md               # 总入口（本文件）
├── CMakeLists.txt           # 顶层 CMake + rtest_add_target() helper
├── common.h                 # 共享 helper（通路无关，header-only）
│
├── examples/                # 端到端示例（教学价值：演示 API 调用）
│   ├── gds/
│   │   ├── gds_put_example.cpp        # GDS 单步 PUT
│   │   ├── gds_get_example.cpp        # GDS PUT + GET 读回验证
│   │   └── gds_multipart_example.cpp  # GDS 分段上传
│   └── rdma/
│       └── rdma_put_example.cpp       # RDMA 单步 + 分段 PUT
│
├── bench/                   # 性能基准（严格度量、可复现）
│   ├── README.md            # bench 详细说明
│   ├── BENCH_REPORT.md      # 历史性能报告
│   ├── harness.h            # 共享基础设施（BaseArgs/RoundResult/PrintReport）
│   ├── gds/
│   │   ├── gds_put_bench.cpp          # GDS 单步 PUT 基准
│   │   └── gds_multipart_bench.cpp    # GDS 分段上传基准
│   └── rdma/
│       ├── rdma_put_bench.cpp         # RDMA 单步 PUT 基准
│       └── rdma_multipart_bench.cpp   # RDMA 分段上传基准
│
├── regression/              # 正确性回归（PASS/FAIL，独立可执行）
│   ├── gds/
│   │   ├── test_multipart_invalid_part_size.cpp
│   │   ├── test_multipart_part_number_violation.cpp
│   │   ├── test_multipart_single_part.cpp
│   │   ├── test_get_single_block_crc.cpp
│   │   └── test_get_multi_block_hash.cpp
│   └── rdma/
│       ├── test_multipart_invalid_part_size.cpp
│       ├── test_multipart_part_number_violation.cpp
│       └── test_multipart_single_part.cpp
│
└── scripts/                 # 运行脚本（一键跑测试/bench）
    ├── run-all.sh
    ├── run-gds-bench.sh
    ├── run-gds-example.sh
    ├── run-rdma-bench.sh
    └── run-rdma-regression.sh
```

## 三类测试的定位

| 类别 | 目录 | 目的 | 输出 |
|------|------|------|------|
| **examples** | `examples/` | 演示 API 用法，教学参考 | 人类可读日志 |
| **bench** | `bench/` | 性能测量，可复现 | 表格 / CSV |
| **regression** | `regression/` | 正确性验证 | `[PASS]` / `[FAIL]` |

## 快速开始

### 构建

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j8
```

### 运行示例

```bash
# GDS 单步 PUT
build/rtest/examples/gds/us3_turbo_gds_put_example --size 4M

# RDMA 单步 PUT
build/rtest/examples/rdma/us3_turbo_rdma_put_example --size 100M --concurrency 8 --reps 50

# GDS PUT + GET 验证
build/rtest/examples/gds/us3_turbo_gds_get_example --single-size 4M --part-size 5M --num-parts 3
```

### 运行基准

```bash
# GDS 单步
build/rtest/bench/gds/us3_turbo_bench_gds_put --size 100M --count 100 --concurrency 8

# RDMA 分段
build/rtest/bench/rdma/us3_turbo_bench_rdma_multipart --total 256M --part-size 4M --concurrency 8 --reps 3
```

### 运行回归测试

```bash
# 一键全部
./scripts/run-all.sh

# 或单独通路
./scripts/run-gds-regression.sh   # 或直接跑各二进制
./scripts/run-rdma-regression.sh
```

## 构建系统

顶层 `CMakeLists.txt` 提供 `rtest_add_target()` helper 函数，消除子目录中重复的
`target_include_directories` / `target_link_libraries` 样板：

```cmake
rtest_add_target(<name> <source> [NEEDS_CUDA] [NEEDS_RDMA])
```

- `NEEDS_CUDA`：链 `CUFILE_INCLUDE_DIR` + `CUDART_LIBRARY`（GDS 通路）
- `NEEDS_RDMA`：链 `IBVERBS_LIBRARY` + `RDMACM_LIBRARY`（RDMA 直调 API 的
  example / bench）
- 不加标志：仅链 `Us3Turbo::client`（纯 host 逻辑 / regression）

## 命名约定

| 类别 | 命名模式 | 示例 |
|------|----------|------|
| example | `us3_turbo_<path>_<op>_example` | `us3_turbo_gds_put_example` |
| bench | `us3_turbo_bench_<path>_<op>` | `us3_turbo_bench_rdma_multipart` |
| regression | `us3_turbo_rtest_<path>_<scenario>` | `us3_turbo_rtest_gds_multipart_single_part` |

## 共享 helper（common.h）

`common.h` 提供通路无关工具，所有 example / bench / regression 统一使用：

| 函数 | 用途 |
|------|------|
| `rtest::ParseSize(s, out)` | 解析 "16M"/"4M"/"1G" 等（1024 进制）|
| `rtest::HumanBytes(b)` | 字节数 → 人类可读（"1.50 GiB"）|
| `rtest::FillHostPattern(buf, offset)` | 确定性 i%251 pattern 填充 |
| `rtest::VerifyHostBuffer(read, size, expected, tag)` | 逐字节比对 + 失配报告 |
| `rtest::MakeTimestampSuffix()` | epoch 秒时间戳字符串 |
| `rtest::kDefaultPartSize` | 4 MiB（与 proxy multipart_part_size 一致）|
