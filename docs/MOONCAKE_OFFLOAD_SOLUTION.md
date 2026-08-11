# Mooncake 冷层接入 Us3Turbo 方案

> 2026-08-11。Mooncake 是 kvcache-ai 的 KVCache 存储引擎；Us3Turbo 是 GDS+RDMA 双通路
> 对象存储。本文说明把 Us3Turbo 作为 Mooncake offload 冷层的背景、痛点、方案、接入方式、优劣。

## 1. 背景

Mooncake 是 LLM serving 的分布式 KVCache 存储（FAST'25 Best Paper），核心是**分层存储 + Transfer Engine**：

```
GPU/DRAM(热, segment)  ──offload_on_evict──►  本地 NVMe(冷, L2)  ──promotion_on_hit──► 回 DRAM
```

- **热层**：client 挂 DRAM/GPU segment，存热 KV。驱逐时不丢，下沉到 L2。
- **L2 冷层**：`StorageBackendInterface`（现成实现 FilePerKey / Bucket / OffsetAllocator / SPDK / HF3FS，**全是本地盘或集群内 FS**）。
- 驱逐后 `promotion_on_hit`（Get 命中盘上 key）异步 SSD-read + RDMA-write 回 DRAM。

"KV cache" = prefill 阶段对每个 token 算出的 K/V 张量，后续每步 decode 都要 attend 回它——**是 GPU 算力的凝结产物，丢了要重跑 prefill**。

## 2. 痛点

1. **冷层是节点本地 NVMe**：节点被回收/崩溃/维护 → offload 的 KV 跟着丢；autoscaler 一回收，idle 用户的昂贵 prefix 全没。
2. **idle 大 prefix 被驱逐后只能重算 prefill**：70B×32k ≈ 10GB 的 prefix，重算 ~3s 且**独占 GPU**；serve 吞吐受损。
3. **不跨集群**：A 集群算的 prefix 无法给 B 集群复用（global prefix cache / 跨区调度做不到）。
4. **容量受单盘限**：长上下文 × 多用户，本地盘 TB 级很快爆；HBM/DRAM 更小。
5. **数据与进程强耦合**：本地盘/KV 生命周期绑死节点，用户跨天续会话做不到。

> 一句话：Mooncake 有 offload→回填的"空间换时间"机制，但冷层缺一层 **durable + 跨集群 + 可扩容**的远端存储。

## 3. 方案

在 `StorageBackendInterface` 加一个 Us3Turbo 实现 `Us3TurboStorageBackend`，作为**最外层 durable 冷层**接在本地 NVMe 之外：

```
GPU/DRAM(segment,热)  ──offload──►  本地 NVMe(L2,快但节点本地)  ──溢出/镜像──►  Us3Turbo 对象存储(durable 冷层)
        ▲                                                                                    │
        └────────────────────── Get / RDMA 回填（不占 GPU）──────────────────────────────────┘
```

- **CPU 产物（host slice）走 RDMA**：`ibv_reg_mr` → backend 拉，host ptr 直送。
- **GPU 产物（device slice）走 GDS**：cuObj token → backend 拉 GPU 显存。
- 数据**不跨 PCIe 停 host、不经本地文件**；小对象(≤4M 单 slice)单步 PUT 零拷贝，大对象 multipart 把 slices coalesce 进 part_size 暂存（同介质拷贝：DRAM→DRAM 或 D2D）。
- 落到 Us3Turbo = 落到 ufile-ac durable NVMe+索引（proxy:9100 / backend:24000），跨节点/跨集群存活。

## 4. 如何接入 Us3Turbo

适配器把 Mooncake 接口映射到 Us3Turbo client（原样复用 `libus3_turbo_client.a`，零 Us3Turbo 改动）：

```
Mooncake Slice{ptr,size}
   │
   ▼
Us3TurboStorageBackend : StorageBackendInterface
   │  BatchOffload(key, slices)  →  ≤4M 单slice: PutObject{Gds,Rdma}(ptr)        [零拷贝]
   │                               大/多slice:   CreateMultipart → UploadPart{Gds,Rdma} ×N → Complete
   │  BatchLoad(key, slice)      →  StatObject → GetObject{Gds,Rdma} 直写 slice.ptr
   │  IsExist(key)               →  StatObject
   ▼ (brpc 控制面，元数据+token，零字节)
us3_turbo_proxy :9100  ──►  ufile-ac :24000 (durable)
```

**代码**（Mooncake 仓）：
- `mooncake-store/include/storage/us3turbo/us3_turbo_storage_backend.{h,cpp}` — 适配器
- `mooncake-store/src/storage/us3turbo/us3turbo_backend_mock_test.cpp` — 档1 mock
- `mooncake-store/src/storage/us3turbo/BUILD.md` — 接线+踩坑

**CMake 接线**：Mooncake 顶层 `option(USE_US3TURBO)`；`mooncake-store/src/CMakeLists` 两处插入——`add_subdirectory(Us3Turbo)` 复用其 CMake 与自带依赖（brpc/protobuf/abseil/cuobjclient/cudart），link `Us3Turbo::client`，加 mock test target。

**构建/运行**（本机 192.168.1.198，已验证）：
```bash
cd Mooncake
PATH=/usr/local/go/bin:$PATH GOTOOLCHAIN=local GOPROXY=https://goproxy.cn,direct \
cmake -S . -B build -DUSE_US3TURBO=ON \
  -DFUSION_ACCESS_DEPS_ROOT=.../Us3Turbo/third_party/install-glog \
  -DBRPC_STATIC_LIBRARY=.../install-glog/brpc-1.11.0-static/lib/libbrpc.a \
  -DWITH_STORE_RUST=OFF -DWITH_RUST_EXAMPLE=OFF -DWITH_P2P_STORE=OFF -DWITH_STORE_GO=OFF
cmake --build build -j$(nproc) --target us3turbo_backend_mock_test
US3TURBO_ENDPOINT=192.168.1.198:9100 US3TURBO_BUCKET=mooncake-offload \
US3TURBO_USE_GDS=0 US3TURBO_RDMA_BIND_IP=192.168.1.198 US3TURBO_PART_SIZE=4194304 \
LD_LIBRARY_PATH=/usr/local/cuda/lib64 ./build/mooncake-store/src/us3turbo_backend_mock_test
# US3TURBO_USE_GDS=1 走 GPU/device 通路
```

> Us3Turbo 的 brpc 默认 `WITH_GLOG=OFF`，与 Mooncake 的 glog 双 'v' gflags 冲突 → 需在独立 `install-glog/` 用 `WITH_GLOG=ON` 重编 brpc（不动共享 brpc）。

## 5. 优势与不足

### 优势

| 优势 | 说明 |
|---|---|
| **续命 idle 会话** | idle KV 下沉 durable 介质，下次按需回填，不重算 prefill——空间换 GPU 时间 |
| **节点/集群回收不丢** | 数据跨节点/跨重启/跨集群存活，autoscaler/维护/崩溃都不影响 |
| **跨集群共享** | A 集群算的 prefix，B 集群直接拉（global prefix cache / 跨区调度） |
| **容量可扩** | 本地盘 TB 级，对象存储 PB 级便宜冷池，满了就溢 |
| **省 GPU** | restore 是网络密集，不占 GPU；重算独占 GPU。即便 wall-clock 相当，吞吐净赚 |
| **GPU 直送 durable** | GDS 通路让 GPU 上 KV 一次落定对象存储，省 `GPU→host→本地文件→再上传` |
| **成本** | HBM > DRAM > NVMe > 对象存储 $/GB；贵重算出来的低频数据放最便宜介质 |

### 不足 / 边界

| 不足 | 说明 |
|---|---|
| **延迟地板高** | 只适合**冷层**（驱逐后续用），热 KV 必须留 DRAM；ms~s 级，不能抢热层 |
| **restore 经济学要量化** | 仅当"重算 > restore"才赚：长 prefix(1-10G+) 赚，短 prefix 重算更快不该存 |
| **无 Delete/List** | Us3Turbo client 现无 Delete/List → `RemoveAll` no-op、`ScanMeta` 未实现（需补 client/proxy 接口） |
| **接入摩擦** | 需重编 glog-brpc 避双 'v'；part_size 须匹配 proxy flag；GDS 路径需 USE_CUDA 门控 |
| **粒度依赖 allocator** | Mooncake 大对象按 slice 数取决于 cachelib(≤4M slab)/offset_allocator；适配器 1 对象=1 Us3Turbo 对象（multipart 聚合） |

## 当前状态

- ✅ 适配器 + mock 编译/链接通过（Mooncake 全量 build + Us3Turbo client 接入）
- ✅ **RDMA/host 通路 mock 3/3**（2M 单步×2 + 20M multipart，offload→load→memcmp 字节一致）
- ✅ **GDS/device 通路 mock 3/3**（同上，GPU 显存直送 durable）
- ⏳ 待续：档2 接 Mooncake master+client，开 `offload_on_evict`+`promotion_on_hit` 触发真实 eviction→Us3Turbo 回填，并量化"10GB 回填 vs 重算 prefill"经济学
