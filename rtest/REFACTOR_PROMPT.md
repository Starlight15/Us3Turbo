# rtest 目录重构优化提示词

**用途**: 喂给代码重构 agent（Claude/Codex/Cursor），一次性把 rtest/ 从当前混乱状态整理成清晰、可扩展、可维护的测试体系。

---

## 一、当前状态诊断

### 1.1 目录结构（现状）

```
rtest/
├── CMakeLists.txt
├── common.h                          # 共享 helper（ParseSize/FillHostPattern/VerifyHostBuffer/...）
├── run_all.sh
├── RDMA_TEST_GUIDE.md
├── TEST_FINDINGS.md
├── examples/
│   ├── CMakeLists.txt
│   ├── gds/
│   │   ├── gds_put_example.cpp          # ✅ GDS 单步 put 示例
│   │   ├── gds_get_example.cpp          # ✅ GDS 单步 get 示例
│   │   ├── gds_multipart_example.cpp    # ✅ GDS 分段上传示例
│   │   ├── gds_bench_example.cpp        # ❌ bench 混入 examples
│   │   ├── multipart_bench_example.cpp  # ❌ bench 混入 examples
│   │   └── rdma_put_example.cpp         # ❌ RDMA 错放在 gds/ 目录下
│   ├── test_gds_bench.sh                # ❌ shell 散落在 examples/ 根
│   ├── test_gds_put.sh
│   └── test_ucx_put.sh
├── bench/
│   ├── CMakeLists.txt
│   ├── BENCH_REPORT.md
│   ├── README.md
│   └── multipart_bench.cpp              # ❌ 仅 GDS，硬编码 #error "BENCH_GDS must be defined"
└── regression/                          # ✅ 结构合理，可作标杆
    ├── CMakeLists.txt
    ├── gds/
    │   ├── CMakeLists.txt
    │   ├── test_get_multi_block_hash.cpp
    │   ├── test_get_single_block_crc.cpp
    │   ├── test_multipart_invalid_part_size.cpp
    │   ├── test_multipart_part_number_violation.cpp
    │   └── test_multipart_single_part.cpp
    └── rdma/
        ├── CMakeLists.txt
        ├── test_multipart_invalid_part_size.cpp
        ├── test_multipart_part_number_violation.cpp
        └── test_multipart_single_part.cpp
```

### 1.2 问题清单

#### 结构性问题（P0）
1. **RDMA example 错放位置**: `examples/gds/rdma_put_example.cpp` 应在 `examples/rdma/`
2. **bench 混入 examples**: `gds_bench_example.cpp` 和 `multipart_bench_example.cpp` 不应在 examples/
3. **RDMA 测试体系缺失**: 无 `examples/rdma/`、无 RDMA bench、无 RDMA get/multipart example
4. **bench 无通路抽象**: `rtest/bench/multipart_bench.cpp` 硬编码 `#error "BENCH_GDS must be defined"`，无法跑 RDMA bench
5. **shell 脚本散落**: `test_*.sh` 散在 examples/ 根目录，应集中到 `scripts/` 或删除

#### 代码质量问题（P1）
6. **bench 三处重复**: `bench/multipart_bench.cpp`、`examples/gds/gds_bench_example.cpp`、`examples/gds/multipart_bench_example.cpp` 三处 bench 逻辑各自为政，Args 解析/统计/输出全部复制粘贴
7. **bench 无阶段划分**: 单文件 400+ 行，没有 setup / warmup / measure / report 的清晰分界
8. **bench 无共享 harness**: 每个 bench 自己写 Percentile、Median、HumanBytes、Args 解析，应抽到 `rtest/bench/harness.h`
9. **common.h 注释误导**: 文件头注释只说"GDS 测试工具"，但实际是通路无关的共享 helper

#### 可维护性问题（P2）
10. **CMakeLists 散落**: 每个子目录一份，命名/目标不统一
11. **命名不一致**: `gds_bench_example` vs `multipart_bench_example` vs `bench_gds_multipart`
12. **regression 标杆未被复用**: regression 的 `[PASS]/[FAIL]`、timestamp key、`rtest::` helper 等最佳实践没扩散到 examples/bench

#### 过期引用（UCX 删除后遗症，P0）

git commit `ec518d0 "删除所有 UCX 代码"` 已移除 UCX 源码，但以下残留引用未清理，会误导读者或导致脚本空跑：

13. **`rtest/run_all.sh`** — 第二段跑 `regression/ucx/` 目录和 `us3_turbo_rtest_ucx_*` 二进制，目录和二进制都已不存在，脚本静默空跑
14. **`rtest/examples/test_ucx_put.sh`** — 调用 `us3_turbo_ucx_put_example`，CMakeLists 里根本没有这个 target，脚本 100% 失败
15. **`rtest/bench/BENCH_REPORT.md`** — 通篇以 "GDS vs UCX" 框架叙述，提到 `us3_turbo_bench_ucx_multipart`（已不构建）
16. **`rtest/bench/README.md`** 和 **`bench/CMakeLists.txt`** 头部注释仍暗示"两个独立可执行 / 多通路"，实际只剩 GDS
17. **`bench/multipart_bench.cpp`** 的 `#if defined(BENCH_GDS) / #error "BENCH_GDS must be defined"` 通路选择机制是 UCX 时代产物，UCX 删除后已成 vestigial 代码（只剩一个通路，宏选择毫无意义）
18. **`bench/BENCH_REPORT.md` / `README.md` / `multipart_bench.cpp` 帮助文本** 对默认 part_size 的表述相互矛盾（有的写 4M，有的写 16M；实际代码用 `kDefaultPartSize=4M`）
19. **`rtest/examples/test_gds_put.sh`** 文件头注释仍写 `test_gds_pimpl.sh`（错名字）

#### common.h 未被充分复用（P1）

`rtest/common.h` 明确提供通路无关 helper，但 **examples/ 下 6 个 cpp 文件一个都没 include 它**：

20. `ParseSize` 在 6 个 example 文件中各自内联重写（`gds_put_example.cpp`、`gds_get_example.cpp`、`gds_multipart_example.cpp`、`gds_bench_example.cpp`、`multipart_bench_example.cpp`、`rdma_put_example.cpp`）
21. `HumanBytes` 在 4 个文件中重复实现
22. `gds_get_example.cpp`（518 行）重新实现了 `ParseSize`/`HumanBytes`/`FillPattern`/`VerifyGet`，但 common.h 都有对应版本
23. 三个 GDS bench 文件（`bench/multipart_bench.cpp` 除外）各自复制 `StartSetter`/`WorkerStats`/`Percentile`/`Median`/`Mean` 工具

#### 细节一致性问题（P2）

24. **`gds_put_example.cpp` 缺 `--proxy` 参数**：hardcode `192.168.1.198:9100`，其他所有 example 都有 `--proxy` flag
25. **`rdma_put_example.cpp` 的 `CompleteMultipartUpload` 传空 parts**：`client.CompleteMultipartUpload(upload_id, {}, cmpl)`，依赖服务端重组；但 GDS 路径传显式 parts（`std::vector<Client::PartInfo>`）——两条通路行为不一致
26. **`gds_get_example.cpp` 是 examples 中结构最好的**（子测试函数命名、字节验证、cuObj descriptor 生命周期 gotcha 文档化），本应作为 example 标杆，但它不 include common.h

---

## 二、目标结构

```
rtest/
├── CMakeLists.txt                         # 顶层，add_subdirectory 各模块
├── common.h                               # 通路无关的共享 helper（扩展现有）
├── README.md                              # 总入口：目录说明 + 如何跑测试
│
├── examples/                              # 演示如何使用 client API（教学价值）
│   ├── CMakeLists.txt
│   ├── gds/
│   │   ├── CMakeLists.txt
│   │   ├── gds_put_example.cpp            # GDS 单步 put
│   │   ├── gds_get_example.cpp            # GDS 单步 get
│   │   └── gds_multipart_example.cpp      # GDS 分段上传
│   └── rdma/
│       ├── CMakeLists.txt
│       ├── rdma_put_example.cpp           # 从 gds/ 移过来
│       ├── rdma_get_example.cpp           # 新增（如 GET 走 RDMA 已支持）或占位
│       └── rdma_multipart_example.cpp     # 新增（或合并到 rdma_put_example）
│
├── bench/                                 # 性能基准（严格度量、可复现）
│   ├── CMakeLists.txt
│   ├── README.md                          # bench 使用说明 + 报告格式
│   ├── BENCH_REPORT.md                    # 现存报告保留
│   ├── harness.h                          # 新增：共享 bench 基础设施
│   │                                      #   - Args 解析框架（统一 proxy/size/concurrency/reps/warmup/csv）
│   │                                      #   - 统计工具（Median/Percentile/AggregateStats）
│   │                                      #   - RoundResult 结构体（统一一轮结果）
│   │                                      #   - Report 输出（table + 可选 CSV）
│   │                                      #   - LatencyTracer（warmup 分离、阶段计时）
│   ├── gds/
│   │   ├── CMakeLists.txt
│   │   ├── gds_put_bench.cpp              # 单步 PUT 基准（源自 gds_bench_example.cpp）
│   │   └── gds_multipart_bench.cpp        # 分段上传基准（源自 multipart_bench.cpp）
│   └── rdma/
│       ├── CMakeLists.txt
│       ├── rdma_put_bench.cpp             # 新增：RDMA 单步 PUT 基准
│       └── rdma_multipart_bench.cpp       # 新增：RDMA 分段上传基准
│
├── regression/                            # 正确性回归（保持现有结构）
│   ├── CMakeLists.txt
│   ├── gds/
│   │   ├── CMakeLists.txt
│   │   └── test_*.cpp                     # 现有保留
│   └── rdma/
│       ├── CMakeLists.txt
│       └── test_*.cpp                     # 现有保留
│
└── scripts/                               # 跑测试/基准的脚本（集中管理）
    ├── run-rdma-regression.sh             # 从 scripts/ 移过来（已存在）
    ├── run-rdma-bench.sh                  # 从 scripts/ 移过来（已存在）
    ├── run-gds-regression.sh              # 新增
    └── run-all.sh                         # 从 rtest/run_all.sh 整合
```

---

## 三、重构任务清单（按优先级）

### Phase 1: 结构性搬迁 + 过期引用清理（P0，先做这些）

- [ ] **P0-1**: 创建 `rtest/examples/rdma/` 目录
- [ ] **P0-2**: 移动 `rtest/examples/gds/rdma_put_example.cpp` → `rtest/examples/rdma/rdma_put_example.cpp`
- [ ] **P0-3**: 移动 bench 文件到 bench 目录：
  - `examples/gds/gds_bench_example.cpp` → `bench/gds/gds_put_bench.cpp`
  - `examples/gds/multipart_bench_example.cpp` → `bench/gds/gds_multipart_bench.cpp`
- [ ] **P0-4**: 清理过期 UCX 引用：
  - 删除 `examples/test_ucx_put.sh`（二进制早就不存在）
  - 重写 `rtest/run_all.sh`：UCX 段改为 RDMA，或删除（让 `scripts/run-rdma-regression.sh` + 新增 `scripts/run-gds-regression.sh` 取代）
  - 重写 `bench/BENCH_REPORT.md` 标题/框架，去掉 GDS-vs-UCX 叙事，更新为现状描述
  - 修正 `bench/README.md` 和 `bench/CMakeLists.txt` 头部注释（去掉"多通路/两个可执行"的暗示）
  - 清理 `bench/multipart_bench.cpp` 的 `#if defined(BENCH_GDS) / #error` 通路选择机制（只剩一个通路，直接去掉宏守卫）
  - 统一 part_size 文档：bench 帮助文本 / README / BENCH_REPORT 与实际代码一致（4M）
- [ ] **P0-5**: 移动 shell 脚本到 `scripts/`：
  - `examples/test_gds_bench.sh` → `scripts/run-gds-bench.sh`（同时修正文件名引用）
  - `examples/test_gds_put.sh` → `scripts/run-gds-example.sh`（同时修正文件头注释的 `test_gds_pimpl.sh` 错名）
- [ ] **P0-6**: 更新所有 CMakeLists.txt 的目标名和路径
- [ ] **P0-7**: 修正 `common.h` 文件头注释（去掉"GDS 测试工具"字样，说明通路无关）

### Phase 1.5: CMake 重构（P1，新增）

当前 CMakeLists 在 ~13 处重复相同的 `target_include_directories` / `target_link_libraries` 模板。重构为 helper 函数：

- [ ] 在 `rtest/CMakeLists.txt` 顶部定义 helper：
  ```cmake
  # rtest_add_target(<name> <source> [NEEDS_CUDA] [NEEDS_RDMA])
  function(rtest_add_target name source)
    cmake_parse_arguments(ARG "NEEDS_CUDA;NEEDS_RDMA" "" "" ${ARGN})
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE us3_turbo_client pthread)
    if(ARG_NEEDS_CUDA)
      target_link_libraries(${name} PRIVATE ${CUDA_LIBRARIES} ${CUFILE_LIBRARY})
      target_include_directories(${name} PRIVATE ${CUDA_INCLUDE_DIRS})
    endif()
    if(ARG_NEEDS_RDMA)
      target_link_libraries(${name} PRIVATE ${IBVERBS_LIBRARY} ${RDMACM_LIBRARY})
    endif()
  endfunction()
  ```
- [ ] 所有 `examples/`、`bench/`、`regression/` 子目录 CMakeLists 改用此 helper
- [ ] 校验 GDS target 链 CUDA，RDMA target 链 ibverbs/rdmacm，纯逻辑 target 什么都不链

### Phase 2: bench harness 抽象（P1，核心重构）

- [ ] **P1-1**: 新建 `rtest/bench/harness.h`，从三个 bench 文件中提取共性：
  ```cpp
  namespace rtest::bench {

  // 统一参数结构（所有 bench 共用字段，通路特定字段用派生）
  struct BaseArgs {
    std::string proxy{"192.168.1.198:9100"};
    std::uint64_t total{64ULL * 1024 * 1024};
    std::uint32_t reps{5};
    std::uint32_t warmup{1};
    std::uint32_t concurrency{1};
    std::string bucket{"bench"};
    std::string key_prefix{"bench"};
    bool verify_crc32c{false};
    bool csv{false};
  };

  // 单轮结果
  struct RoundResult {
    double setup_ms{0};
    double data_plane_ms{0};    // 纯数据传输
    double control_plane_ms{0}; // Create/Complete 等控制面
    double total_ms{0};
    std::uint64_t bytes{0};
    bool ok{false};
    std::string error;
  };

  // 统计聚合
  struct AggregateStats {
    std::size_t n{0};
    double min_ms{0}, p50_ms{0}, p95_ms{0}, max_ms{0}, mean_ms{0};
    double throughput_mibps{0};
  };

  // 工具函数
  AggregateStats ComputeStats(const std::vector<RoundResult>& rounds);
  void PrintReport(std::string_view bench_name, const std::vector<RoundResult>& rounds,
                   const AggregateStats& stats);
  void WriteCsv(std::string_view path, const std::vector<RoundResult>& rounds);

  // Bench runner 模板（通路无关）
  template <typename RunFn>
  std::vector<RoundResult> RunBench(const BaseArgs& args, RunFn&& run_once);

  }  // namespace rtest::bench
  ```
- [ ] **P1-2**: 重构 `bench/gds/gds_put_bench.cpp` 使用 harness
- [ ] **P1-3**: 重构 `bench/gds/gds_multipart_bench.cpp` 使用 harness
- [ ] **P1-4**: 每个 bench 文件强制遵循以下结构：
  ```
  // 文件头注释：说明测什么、关键测量维度、用法示例
  #include "rtest/bench/harness.h"

  namespace {
  // 1. 通路特定的 Args 派生（如有额外字段）
  struct Args : rtest::bench::BaseArgs { /*...*/ };

  // 2. 解析参数（只处理通路特定部分，BaseArgs 由 harness 处理）
  Args ParseArgs(int argc, char** argv);

  // 3. 单轮执行函数（harness 负责 warmup 过滤、计时、统计）
  rtest::bench::RoundResult RunOnce(const Args& args, /*...*/);
  }  // namespace

  int main(int argc, char** argv) {
    // 4. 标准流程：解析 → 初始化 client → RunBench → 输出报告 → 清理
  }
  ```

### Phase 3: 补齐 RDMA bench（P1，新增）

- [ ] **P2-1**: 新建 `bench/rdma/rdma_put_bench.cpp`
  - 基于 `rdma_put_example.cpp` 的并发路径改造
  - 复用 harness
  - 测量：descriptor 注册延迟 + RDMA PUT 延迟 + CRC32C 校验开销
- [ ] **P2-2**: 新建 `bench/rdma/rdma_multipart_bench.cpp`
  - 对标 `bench/gds/gds_multipart_bench.cpp`，通路换成 RDMA
  - 测量：Create / ΣUploadPart / Complete 阶段耗时

### Phase 4: 补齐 RDMA examples + common.h 强制复用（P2）

- [ ] **P3-1**: 评估是否需要 `rdma_get_example.cpp`
  - 当前 RDMA 仅 PUT，GET 走 GDS → 可加占位文件说明（或 `README.md` 注明 "RDMA GET 不支持，参见 GDS"）
- [ ] **P3-2**: 评估 `rdma_multipart_example.cpp` 与 `rdma_put_example.cpp` 是否合并
  - 现状：`rdma_put_example.cpp` 已包含 `--multipart` 模式（单文件两用）
  - 决策：保留单文件 but 拆分 clearer CLI help，或拆成两个文件
- [ ] **P3-3**: 强制所有 example/bench cpp 文件 `#include "rtest/common.h"`，删除内联重复的 `ParseSize` / `HumanBytes` / `FillPattern` 实现。检查清单：
  - `gds_put_example.cpp` — 删内联 `ParseSize`
  - `gds_get_example.cpp` — 删内联 `ParseSize`/`HumanBytes`/`FillPattern`/`VerifyGet`，用 `rtest::VerifyHostBuffer`
  - `gds_multipart_example.cpp` — 删内联 `ParseSize`/`HumanBytes`
  - `rdma_put_example.cpp` — 删内联 `ParseSize`
- [ ] **P3-4**: 修复细节一致性问题：
  - `gds_put_example.cpp` 加 `--proxy` flag（与其他所有 example 对齐）
  - `rdma_put_example.cpp` 的 `CompleteMultipartUpload` 改为传显式 parts（与 GDS 路径对齐）

### Phase 5: 文档整合（P2）

- [ ] **P4-1**: 新建 `rtest/README.md` 作为总入口
  - 目录结构说明
  - 三类测试的定位：examples（教学）/ bench（性能）/ regression（正确性）
  - 快速运行命令
- [ ] **P4-2**: 更新 `rtest/bench/README.md` 反映新结构
- [ ] **P4-3**: 在 `rtest/bench/harness.h` 顶部写清楚使用说明

---

## 四、质量门禁（重构完成时必须满足）

### 4.1 编译与运行
- [ ] `cmake --build build --target all` 无编译错误
- [ ] 所有 bench 二进制：`us3_turbo_bench_gds_put`、`us3_turbo_bench_gds_multipart`、`us3_turbo_bench_rdma_put`、`us3_turbo_bench_rdma_multipart` 可执行
- [ ] 所有 example 二进制可执行
- [ ] 所有 regression 二进制可执行
- [ ] `scripts/run-all.sh` 能一键跑所有测试

### 4.2 代码质量
- [ ] 每个 bench 文件 ≤ 300 行（harness 吸收共性后）
- [ ] 每个 example 文件 ≤ 250 行
- [ ] 每个 regression 文件 ≤ 250 行（保持现有标杆）
- [ ] 无 `#error "BENCH_GDS must be defined"` 这类硬编码通路假设
- [ ] bench 三件套（Args/RoundResult/Stats）100% 来自 `harness.h`
- [ ] 所有 cpp 文件使用 `rtest::` 命名空间的 helper（`ParseSize`、`HumanBytes`、`FillHostPattern` 等）
- [ ] **0 处过期 UCX 引用**：`grep -r ucx rtest/` 和 `grep -r UCX rtest/` 应当为空（或仅剩 `docs/rdma-libibverbs-spec.md` 这种历史说明）
- [ ] **0 处 shell 脚本死链**：所有 `.sh` 中引用的二进制必须存在于 CMakeLists
- [ ] **part_size 文档一致性**：`grep -rn "16M\|4M\|16MB\|4MB\|kDefaultPartSize" rtest/bench/` 应全部指向 4M

### 4.3 命名一致性
- [ ] example 命名：`<path>_<operation>_example.cpp`（gds_put_example / rdma_put_example）
- [ ] bench 命名：`<path>_<operation>_bench.cpp`（gds_put_bench / rdma_multipart_bench）
- [ ] regression 命名：`test_<scenario>.cpp`（保持不变）
- [ ] CMake 目标命名：`us3_turbo_<category>_<path>_<op>`（us3_turbo_bench_rdma_put）

### 4.4 文档完整
- [ ] 每个文件顶部注释说清楚：测什么 / 怎么跑 / 关键测量维度
- [ ] `rtest/README.md` 与 `rtest/bench/README.md` 反映最终结构

---

## 五、重构红线（禁止事项）

- ❌ **不要改动 `client/`、`proxy/`、`proto/` 任何业务代码** — 本次只重构 rtest
- ❌ **不要删除 regression/ 下任何测试** — regression 是标杆，保持原样
- ❌ **不要引入新依赖**（gtest/benchmark 等）— 保持 plain-main 风格
- ❌ **不要改 `rtest/common.h` 的 API** — 只扩展不破坏（可以加新函数，不要改已有函数签名）
- ❌ **不要重写 bench 测量逻辑** — 只做结构重构，测量语义保持不变（避免 bench 历史数据失去可比性）
- ❌ **不要在 harness.h 中引入 CUDA** — harness 必须通路无关，CUDA 相关的东西留在具体 bench 文件里

---

## 六、参考标杆（regression 的良好实践，应扩散）

从 `rtest/regression/rdma/test_multipart_single_part.cpp` 提炼的最佳实践：

1. **文件头注释三要素**：测什么 / 验证什么 / 通路特征
2. **测试命名常量**：`constexpr char kTestName[] = "rdma_multipart_single_part";`
3. **时间戳隔离 key**：`rtest::MakeTimestampSuffix()` 避免多次运行冲突
4. **统一 helper 复用**：`rtest::ParseSize` / `rtest::FillHostPattern` / `rtest::HumanBytes`
5. **setup → execute → verify → cleanup 四段式**：cleanup 用 goto 统一出口
6. **标准化输出**：`[PASS] kTestName` / `[FAIL] kTestName: reason`
7. **幂等 Abort**：cleanup 时 `(void)client.AbortMultipartUpload(...)` 忽略错误

---

## 七、执行顺序建议

推荐按以下顺序执行（每步可独立 commit）：

1. **Commit 1**: 纯文件移动 + UCX 引用清理（Phase 1 全部），不动业务代码逻辑
2. **Commit 1.5**: CMake helper 函数重构（Phase 1.5），纯构建层改动
3. **Commit 2**: 创建 `bench/harness.h`（P1-1），不重构任何 bench
4. **Commit 3**: 重构 `bench/gds/gds_put_bench.cpp` 使用 harness（P1-2）
5. **Commit 4**: 重构 `bench/gds/gds_multipart_bench.cpp` 使用 harness（P1-3）
6. **Commit 5**: 新增 `bench/rdma/rdma_put_bench.cpp`（P2-1）
7. **Commit 6**: 新增 `bench/rdma/rdma_multipart_bench.cpp`（P2-2）
8. **Commit 7**: 补齐 RDMA examples + 强制 common.h 复用 + 修复细节一致性（Phase 4）
9. **Commit 8**: 文档整合（Phase 5）

每个 commit 独立可编译、可运行、可回滚。

---

## 八、验收标准（PR 描述应包含）

```markdown
## 重构概述
- 文件移动 N 个，新增 M 个，删除 K 个
- bench 代码行数变化：before XXX 行 → after YYY 行（harness 复用）

## 结构对比
- before：[粘贴 1.1 现状树]
- after：[粘贴 二、目标结构树]

## 验证
- [ ] cmake --build build --target all 通过
- [ ] 各 bench 在 192.168.1.198:9100 上跑通
- [ ] 各 regression 全部 PASS
- [ ] bench 数据与重构前对比，差异 < 5%（证明没改测量逻辑）

## 兼容性
- 是否影响现有 bench 历史数据？[是/否]
- 是否影响 CI 脚本？[是/否，列出需更新的脚本]
```
