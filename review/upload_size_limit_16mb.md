# 上传大小限制：单步对象 ≤ 16MB / 分段 part ≤ 16MB / 后端 block ≤ 16MB

## 目标

1. **单步上传**：对象大小限制为 **16MB**，超出拒绝（引导走分段）。
2. **分段上传 client 端**：单个 part 最大长度 **16MB**，超出拒绝。
3. **保证写到后端的 block 不超过 16MB**：proxy 切分产物 + backend 收字节双重兜底。

三层各自独立校验（client → proxy → backend），任一层都能挡住超限请求，避免依赖上层。

---

## 约束

1. 只改现有源文件，不新建源文件。
2. 三处统一用同一数值 `16 MiB = 16 * 1024 * 1024`，各模块内定义各自常量（不跨模块共享头，避免耦合）。
3. 校验失败返回明确错误信息，指明上限与建议（单步超限 → 提示用分段）。
4. 现有 4MB block 切分逻辑不动（part ≤16MB 时天然 ≤4 个 block，每 block ≤4MB）；backend 上限改动仅作兜底。

---

## 常量约定

```cpp
// 16 MiB —— 单步对象上限 / 分段 part 上限 / 后端单 block 上限。
constexpr std::uint64_t kMaxUploadBytes = 16ULL * 1024 * 1024;
```

---

## 改动 1：单步上传对象 ≤ 16MB

### 1a. Client 默认上限（`client/include/us3_turbo/client/options.h`）

```cpp
// 单步 PUT 对象上限，默认 16MiB；超出走分段上传。0 表示不限制。
std::size_t put_single_max_bytes{16ULL * 1024 * 1024};
```

> `client/src/client.cpp:157-163` 已有 `put_single_max_bytes` 校验逻辑，改默认值即生效，无需改校验代码。

### 1b. Proxy 兜底校验（`proxy/src/service/proxy_control_plane_service.cpp`）

在 `GdsPut` / `UcxPut` 现有 `object_size() == 0` 校验之后，追加上限校验（两处）：

```cpp
if (request->object_size() > kMaxUploadBytes) {
  cntl->SetFailed(PROXY_ERR_INVALID_PARAM,
                  "object_size exceeds 16MiB single-step limit; use multipart");
  return;
}
```

> `kMaxUploadBytes` 定义在本文件匿名 namespace 内。

---

## 改动 2：分段 client 端 part ≤ 16MB

### 文件：`client/src/client.cpp`

在 `UploadPartGds` 与 `UploadPartUcx` 入口（`initialized_` 校验之后、`AcquireToken/Descriptor` 之前）各加一处：

```cpp
constexpr std::uint64_t kMaxPartBytes = 16ULL * 1024 * 1024;
if (buffer.size > kMaxPartBytes) {
  out_error = "part size " + std::to_string(buffer.size) +
              " exceeds 16MiB per-part limit";
  return false;
}
```

> 空 part（`buffer.size == 0`）是否拒绝按现状保持；本改动只加上限。

---

## 改动 3：后端 block ≤ 16MB 兜底

### 3a. Backend sink 上限（`backend/src/backend_gds_sink.cpp`）

将 `kMaxChunkBytes` 从 1 GiB 收紧到 16 MiB，并更新错误信息：

```cpp
// 单次 RDMA-READ 的 block 上限：16 MiB（分段 block ≤4MB，单步对象 ≤16MB）。
constexpr std::size_t kMaxChunkBytes = 16ULL * 1024ULL * 1024ULL;  // 16 MiB
```

对应 `ReceiveAndDiscard` 里的错误串：
```cpp
if (length > kMaxChunkBytes) {
  outcome.error = "PUT chunk exceeds 16MiB backend limit";
  return outcome;
}
```

> PinnedBufferPool 的 size class（1M/16M/256M/1G）可保持不变（大 class 仅不再被使用，无害）；若要精简可去掉 256M/1G 两档，非必需。
> UCX sink 若有等价上限常量，同步收紧为 16 MiB。

### 3b. Backend block service 显式校验（`backend/src/backend_block_data_plane_service.cpp`）

在 `PutBlock` 内对 `gds_source.block_size()` / `ucx_source.block_size()` 已有的 `== 0` 校验旁，追加上限校验，给出比 sink 更早、更明确的错误：

```cpp
constexpr std::uint64_t kMaxBlockBytes = 16ULL * 1024 * 1024;
if (src.block_size() > kMaxBlockBytes) {
  response->set_ok(false);
  response->set_error_message("block_size exceeds 16MiB limit");
  cntl->SetFailed("block_size exceeds 16MiB limit");
  return;
}
```

---

## 验证

### 单步上传
```bash
# 16MiB 以内：成功
./us3_turbo_gds_put_example --size 16M   # 边界，应成功
# 超 16MiB：client 侧直接拒绝（不发 RPC）
./us3_turbo_gds_put_example --size 17M   # 应报 exceeds put_single_max_bytes
```
- [ ] `buffer.size <= 16MiB` 成功
- [ ] `buffer.size > 16MiB` client 侧 `PutObject` 返回 false，日志提示走分段
- [ ] 绕过 client 直接构造 `object_size > 16MiB` 的 RPC，proxy 返回 `INVALID_PARAM`

### 分段上传
```bash
./us3_turbo_gds_multipart_example --part-size 16M --num-parts 4   # 边界，应成功
./us3_turbo_gds_multipart_example --part-size 17M --num-parts 1   # 应被 client 拒绝
```
- [ ] `part-size <= 16MiB` 全部 part 成功
- [ ] `part-size > 16MiB` `UploadPartGds/Ucx` 返回 false，错误含 "16MiB per-part limit"
- [ ] backend 日志中单 block `size` 恒 ≤ 4MiB（现有切分），且任何 `block_size > 16MiB` 被 backend 拒绝

---

## 影响面小结

| 层 | 文件 | 改动 |
|---|---|---|
| Client 单步 | `options.h` | 默认上限 1G → 16M |
| Proxy 单步 | `proxy_control_plane_service.cpp` | GdsPut/UcxPut 加 `object_size > 16M` 校验 |
| Client 分段 | `client.cpp` | UploadPartGds/Ucx 加 `buffer.size > 16M` 校验 |
| Backend 兜底 | `backend_gds_sink.cpp` | `kMaxChunkBytes` 1G → 16M |
| Backend 兜底 | `backend_block_data_plane_service.cpp` | PutBlock 加 `block_size > 16M` 校验 |
