# Us3Turbo

GDS（GPUDirect Storage）+ RDMA 双通路对象存储。控制面 brpc proxy，客户端双通路 SDK，
数据面后端为 ufile-ac（独立仓 `ggds-compile-env`）。支持单步 PUT、分段上传、GET，
GDS 走 device 显存、RDMA 走 host 内存。

三层拓扑：

```
client SDK / bench  ──brpc──►  us3_turbo_proxy (:9100)  ──TCP/RDMA──►  ufile-ac (:24000 TCP / :18666 GDS cuobj)
```

---

# 第一部分：编译

## 1. 前置依赖

编译依赖一组离线静态库（brpc、protobuf、spdlog、abseil 等），由**源码 tarball**
（`third_party/src/*.tar.gz`）经仓内 `third_party/build_deps.sh` 构建到
`third_party/install/`。不再提交预编译静态产物，跨机可移植。缺该目录时
`./do_make.sh` 会报：

```
Dependency root not found: third_party/install. Run ./do_make.sh --with-dep first ...
```

系统侧还需：libibverbs / librdmacm（RDMA 通路）。GDS 通路另需 CUDA toolkit
（12.6 / 13.x，提供 cudart、`cuda.h`/`cuda_runtime.h`）与 cuObj SDK（`libcufile`、
`libcuobjclient`、`libcuobjserver`），由 `scripts/install_gds_deps.sh` 自动 apt 安装。
C++20 编译器（gcc-11+，devtoolset-11 即可，CMake 强制 `_GLIBCXX_USE_CXX11_ABI=1`
并静态链接 libstdc++/libgcc）。

## 2. 一键编译

推荐用根目录封装脚本 `./do_make.sh`（默认 `RelWithDebInfo`，自动探测 nproc）：

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
./do_make.sh
```

首次在新机器上编译（自动装系统依赖、从源码重编 third_party；加 `--enable-gds` 时
同时 apt 安装 CUDA toolkit + cuObj SDK）：

```bash
./do_make.sh --with-dep            # 只编 RDMA 通路
./do_make.sh --with-dep --enable-gds   # 连 GDS 通路一起，一键装 CUDA/cuObj
```

常用选项：

| 选项 | 作用 |
|---|---|
| `--clean` | 配置前清空 `build/` |
| `--debug` / `--release` / `--relwithdebinfo` | 切换构建类型（默认 RelWithDebInfo） |
| `--with-dep` | 首次编译：装系统依赖 + 源码重编 `third_party/install`（+GDS 时装 CUDA/cuObj SDK） |
| `--cuda-root PATH` | CUDA toolkit 根目录（默认自动探测 `/usr/local/cuda[-<ver>]`） |
| `-j, --jobs N` | 并行编译任务数 |
| `--deps-root PATH` | 覆盖依赖根目录（默认 `third_party/install`） |
| `--enable-gds` / `--disable-gds` | 开启/关闭 GDS（CUDA cuObj）通路编译（默认 OFF） |

环境变量等价：`BUILD_TYPE`、`JOBS`、`BUILD_RTEST`（默认 ON，控制是否编译 rtest 例程/
示例/bench；关掉可加速只出 proxy + client）、`US3_TURBO_ACCESS_ENABLE_GDS`（默认 OFF，
=ON 等价 `--enable-gds`）。GDS 关闭时不查找/链接任何 CUDA/cuobj/cufile 依赖，只出
RDMA（host 内存）通路；无 GPU 机器直接 `./do_make.sh` 即可。

## 3. 手动 CMake（等价于脚本）

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFUSION_ACCESS_DEPS_ROOT="$(pwd)/third_party/install" \
  -DUS3_TURBO_ACCESS_BUILD_RTEST=ON
cmake --build build -j"$(nproc)"
```

GDS 通路：加 `-DUS3_TURBO_ACCESS_ENABLE_GDS=ON`；CUDA 装在非标准路径时再加
`-DUS3_TURBO_ACCESS_CUDA_ROOT=/path/to/cuda`（缺省自动探测 `/usr/local/cuda[-<ver>]`）。

## 4. 产物

| 产物 | 路径 | 说明 |
|---|---|---|
| proxy 可执行 | `build/proxy/us3_turbo_proxy` | 控制面 brpc 服务 |
| client 静态库 | `build/client/libus3_turbo_client.a` | SDK，被 bench/example/rtest 链接 |
| bench | `build/rtest/bench/{gds,rdma}/us3_turbo_bench_*` | 性能基准 |
| example | `build/rtest/examples/{gds,rdma}/us3_turbo_*_example` | 单功能用法示例 |
| 回归测试 | `build/rtest/regression/{gds,rdma}/us3_turbo_rtest_*` | 功能回归 |

单编某目标：

```bash
cmake --build build --target us3_turbo_proxy -j
cmake --build build --target us3_turbo_bench_gds_multipart -j
```

## 5. 增量编译与清理

```bash
cmake --build build -j            # 增量
./do_make.sh --clean              # 全量重编
```

---

# 第二部分：测试与运行

## 1. 启动哪些模块

完整跑一次 bench 需要三个进程，按顺序启动：

| 序号 | 模块 | 角色 | 端点 |
|---|---|---|---|
| ① | **ufile-ac** | 数据面后端（独立仓 `ggds-compile-env`） | `192.168.1.198:24000`（TCP）/ `:18666`（GDS cuobj） |
| ② | **us3_turbo_proxy** | 控制面 brpc（本仓） | `192.168.1.198:9100` |
| ③ | **bench / client** | 客户端进程（本仓 `build/rtest/bench/...`） | 本机 |

> 测试机固定 IP `192.168.1.198`。bench 默认 proxy 端点 `192.168.1.198:9100`
> （`rtest/common.h::kDefaultProxyEndpoint`），可用 `--proxy` 覆盖。

## 2. 启动后端 ufile-ac（①）

后端在 `ggds-compile-env/ufile-ac` 仓，用 ini 配置 + gflags 启动。**性能调参测试用
`--mock-aio-write=1`**：PUT 跳过 NVMe 落盘（`ioContext.cc:424` 不调 SubmitWrite），
只测 `client → proxy → backend RDMA-read` 数据搬运段，不含持久化开销；该模式下 GET
不可用，所以性能调参轮次只测 PUT。

```bash
cd /mnt/us3_test/xinghui.shao/gds/ggds-compile-env/ufile-ac
nohup ./build/ufile-ac \
  --config-file=config/ufile-ac-gds-proxy.ini \
  --mock-aio-write=1 \
  > /tmp/uac_mock.log 2>&1 &
```

- 真实端到端测试（含落盘、GET 可用）去掉 `--mock-aio-write=1`。
- backend `worker_threads`、`[rdma] mock_mode`、`[gds] mock_rdma_read` 由 ini 控制，
  性能测试固定 `worker_threads=8`、`mock_mode=0`、`mock_rdma_read=0`。

## 3. 启动 proxy（②）

proxy 通过 flagfile 启动，配置在 `proxy/conf/proxy.flags`（gflags 格式）。当前默认值
已是性能调优后的最优值（`num_threads=16` / `backend_conn_pool_size=16` /
`multipart_part_size=8M`，见 `docs/*_DEEP_ANALYSIS.md`）。

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
nohup ./build/proxy/us3_turbo_proxy \
  --flagfile=proxy/conf/proxy.flags \
  > /tmp/proxy.log 2>&1 &
```

确认就绪：

```bash
pgrep -f us3_turbo_proxy        # 拿到 pid
tail -n 5 /tmp/proxy.log        # 看到 brpc 监听 :9100
```

### 3.1 proxy 关键 flag

| flag | 默认 | 说明 |
|---|---|---|
| `--proxy_port` | 9100 | 控制面端口 |
| `--bind_host` | 0.0.0.0 | 监听地址（flagfile 内固定 `192.168.1.198`） |
| `--num_threads` | 16 | brpc worker 线程数，须 ≈ `backend_conn_pool_size` |
| `--backend_endpoint` | `192.168.1.198:24000` | 后端 TCP 端点（setid 须与 backend `[common] setid` 一致） |
| `--backend_conn_pool_size` | 16 | proxy→backend 连接池大小 |
| `--multipart_part_size` | 8388608 (8M) | 分段上传 part 大小，同时是落盘 block 大小 |
| `--max_single_put_bytes` | 4M | 单步 PUT 上限，超过必须走 multipart |
| `--log_level` | info（flagfile 内 warn） | `debug/info/warn/error` |

### 3.2 改 proxy 参数并重启

改参流程：编辑 flagfile → kill 旧进程 → nohup 重启。**kill 与 nohup 要分两条命令**，
避免 `kill` 信号打断同一 compound 命令导致 nohup 不执行：

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo

# 改参（三行可按需选）
sed -i 's/^--num_threads=.*/--num_threads=16/'               proxy/conf/proxy.flags
sed -i 's/^--backend_conn_pool_size=.*/--backend_conn_pool_size=16/' proxy/conf/proxy.flags
sed -i 's/^--multipart_part_size=.*/--multipart_part_size=8388608/' proxy/conf/proxy.flags

# 重启（分两步）
kill $(pgrep -f us3_turbo_proxy); sleep 3
nohup ./build/proxy/us3_turbo_proxy --flagfile=proxy/conf/proxy.flags > /tmp/proxy.log 2>&1 &
```

> 也可不改 flagfile，直接命令行覆盖：`./build/proxy/us3_turbo_proxy --flagfile=proxy/conf/proxy.flags --num_threads=8`。

## 4. 跑 bench（③）

### 4.1 分段上传（multipart）—— 性能调参主用

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo

# GDS 分段上传（device 显存）
./build/rtest/bench/gds/us3_turbo_bench_gds_multipart \
  --part-size 8M --total 64M --concurrency 32 --reps 10 --warmup 2

# RDMA 分段上传（host 内存）
./build/rtest/rdma/us3_turbo_bench_rdma_multipart \
  --part-size 8M --total 64M --concurrency 32 --reps 10 --warmup 2
```

bench 参数（gds/rdma multipart 通用）：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--proxy ADDR` | `192.168.1.198:9100` | proxy 端点 |
| `--total SIZE` | 64M | 单对象总大小 |
| `--part-size SIZE` | 8M | part 大小，**须 ≤ 16M**（backend `MAX_VALUE_LENGTH`）且与 proxy `--multipart_part_size` 一致，否则被拒 |
| `--concurrency N` | 1 | worker 线程数（性能测试用 32） |
| `--reps N` | 5 | 每 worker 正式测量轮数 |
| `--warmup N` | 0 | 预热轮数（排除冷启动，性能测试用 2） |
| `--bucket NAME` | test-bucket | bucket |
| `--key-prefix STR` | bench | key 前缀 |
| `--verify-crc32c` | off | 开启 CRC32C 校验 |
| `--trace` | off | 输出 per-part 时延（`acquire/rpc/total/bytes`）；同时把 client `log_level` 降到 info 让 trace 生效 |
| `--csv` | off | 输出 CSV 而非汇总 |

输出：throughput (MiB/s)、wall time (ms)、阶段时延（setup / data-plane / control-plane 的
avg/p50/p95/min/max）、ok/fail 计数。

### 4.2 单步 PUT / GET bench

```bash
# GDS 单步 PUT（size ≤ 4M max_single_put，否则须走 multipart）
./build/rtest/bench/gds/us3_turbo_bench_gds_put \
  --size 4M --count 40 --concurrency 8 --warmup 2

# RDMA 单步 PUT
./build/rtest/bench/rdma/us3_turbo_bench_rdma_put \
  --size 4M --count 40 --concurrency 8 --warmup 2

# RDMA GET（关 mock、有已上传对象时）
./build/rtest/bench/rdma/us3_turbo_bench_rdma_get \
  --size 4M --count 100 --concurrency 8 --warmup 2
```

PUT bench 参数：`--size`（默认 100M，但单步须 ≤ 4M）、`--count`（对象数，默认 10）、
`--concurrency`、`--warmup`、`--bucket`、`--key-prefix`、`--verify-crc32c`、`--trace`。
GET bench 同构，默认 `--size 4M`、`--key-prefix bench-get`；GET bench 内部先串行 PUT
全部对象再并发 GET 测读吞吐，故 count 即播种+读取对象数。

> 注意：`--mock-aio-write=1` 下 GET 不可用（数据未落盘），GET bench 必须在关 mock 后跑。
> bench warmup 统计已修复，`--warmup` 不再虚高吞吐（warmup 字节不计入吞吐分子）。

### 4.3 时延 trace（用于调参分析）

加 `--trace` 后每个 part 打印一行，用于定位固定开销与排队争抢：

```
[gds] part 3/8 acquire=0.8ms rpc=42.1ms total=42.9ms bytes=8388608
```

- `acquire`：client 取 token/MR（GDS 含 cuObj token / MR 注册）。
- `rpc`：proxy 往返（含 proxy 内部排队 + backend 往返）。

结合 `--reps`、`--warmup` 与 proxy log_level=warn，可稳定复现 `docs/*_DEEP_ANALYSIS.md`
中的 per-part 模型。

## 5. 回归测试 / 示例

```bash
# 回归（需后端 + proxy 就绪；GET 类须关 mock）
./build/rtest/regression/gds/us3_turbo_rtest_gds_multipart_single_part
./build/rtest/regression/rdma/us3_turbo_rtest_rdma_get_single_block_crc
# ...其余 rtest_* 同理

# 示例（单功能最小用法）
./build/rtest/examples/gds/us3_turbo_gds_put_example
./build/rtest/examples/rdma/us3_turbo_rdma_multipart_example
```

单块 CRC 回归用显式 2M size（`< 4M` 单步上限），与 `kDefaultPartSize` 解耦，故不带
`--part-size`。回归用 `--proxy` 覆盖端点，`--size` 覆盖单块大小。

## 6. 性能调参复现流程（端到端）

固定 backend mock-on + client 32 线程 + total 64M，扫一个 proxy 参数：

```bash
# 1. 后端 mock-on
cd /mnt/us3_test/xinghui.shao/gds/ggds-compile-env/ufile-ac
nohup ./build/ufile-ac --config-file=config/ufile-ac-gds-proxy.ini \
  --mock-aio-write=1 > /tmp/uac_mock.log 2>&1 &

# 2. proxy 最优配置
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
nohup ./build/proxy/us3_turbo_proxy --flagfile=proxy/conf/proxy.flags > /tmp/proxy.log 2>&1 &

# 3. 跑双通路
./build/rtest/bench/gds/us3_turbo_bench_gds_multipart  --part-size 8M --total 64M --concurrency 32 --reps 10 --warmup 2
./build/rtest/bench/rdma/us3_turbo_bench_rdma_multipart --part-size 8M --total 64M --concurrency 32 --reps 10 --warmup 2

# 4. 改参重测：见 §3.2
```

最优配置（mock-on，client 32 线程，64M）实测：part=8M / nt=16 / cp=16 →
GDS ≈ 3864 MiB/s、RDMA ≈ 6527–6700 MiB/s。完整数据与参数影响分析见
`docs/{PART_SIZE,NUM_THREADS,CONN_POOL}_DEEP_ANALYSIS.md`。

## 6.1 真实读写端到端（mock 关闭）

关 mock 后 PUT 真落盘、GET 可用，覆盖单步 PUT / 分段上传 / 下载 GET（数据见 git 历史，2026-08-08 复测）。

```bash
# 1. 后端真实读写（去掉 --mock-aio-write=1）
cd /mnt/us3_test/xinghui.shao/gds/ggds-compile-env/ufile-ac
nohup ./build/ufile-ac --config-file=config/ufile-ac-gds-proxy.ini \
  > /tmp/uac_real.log 2>&1 &

# 2. proxy 最优配置（同上）
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
nohup ./build/proxy/us3_turbo_proxy --flagfile=proxy/conf/proxy.flags > /tmp/proxy.log 2>&1 &

# 3. 单步 PUT（size ≤ 4M）
./build/rtest/bench/gds/us3_turbo_bench_gds_put  --size 4M --count 40 --concurrency 8 --warmup 2
./build/rtest/bench/rdma/us3_turbo_bench_rdma_put --size 4M --count 40 --concurrency 8 --warmup 2

# 4. 分段上传
./build/rtest/bench/gds/us3_turbo_bench_gds_multipart  --part-size 8M --total 64M --concurrency 32 --reps 10 --warmup 2
./build/rtest/bench/rdma/us3_turbo_bench_rdma_multipart --part-size 8M --total 64M --concurrency 32 --reps 10 --warmup 2

# 5. 下载 GET（RDMA）
./build/rtest/bench/rdma/us3_turbo_bench_rdma_get --size 4M --count 100 --concurrency 8 --warmup 2
```

真实读写实测（3 轮稳定值）：单步 PUT RDMA ≈ 3258、GDS ≈ 1273–1503 MiB/s；
分段 GDS ≈ 1714、RDMA ≈ 3743 MiB/s；下载 RDMA GET ≈ 2513 MiB/s。较 mock 纯搬运
降 43–56%（NVMe 落盘开销），最优点不变。

> GDS buffer pool 扩容优化已证伪:max_per_class=4 已最优,扩容反降 ~27%,pool 非 GDS 瓶颈。
> 注意:测试机根盘满(100%)会拉低真实写 RDMA 数值,测前需清盘。

## 7. 关停

```bash
kill $(pgrep -f us3_turbo_proxy)      # proxy
kill $(pgrep -f ufile-ac)            # 后端
```
