# 阶段 8：ETag / CRC 解耦修复 — 履行"默认关闭"并修占位符 bug

## 背景

复查已实现代码发现两个耦合问题：

1. **Backend CRC 默认开启**，与"backend/client CRC 都默认关闭"的需求相反。
2. **`BuildBlockEtag` 占位符恒返回 `"block-0"`**：注释声称用 `block_no + size` 保证唯一，但代码只用了 `crc`，导致关闭 CRC 时所有 block etag 相同，part etag 与数据内容无关。

**核心认识**：算 content-derived etag（对 block 字节做哈希）与算 CRC32C 是同样开销——都要扫一遍字节。因此"CRC 默认关闭以免扫字节做纯吞吐压测"的前提下，**不可能同时既不扫字节又拿到有意义的 etag**。二者必须解耦：

- **压测态（默认）**：不扫字节；etag 为**显式占位符**（唯一但非内容派生），CRC=0。
- **完整性态（开关开启）**：扫一遍字节算内容哈希；etag = 该哈希，CRC 可同次扫描附带算出。

---

## 约束

1. 只改现有源文件，不新建源文件。
2. 保持单步 `GdsPut/UcxPut` 路径不变。
3. 关闭态下 etag 必须**唯一且良构**（供 proxy 汇总 part etag、CompleteSession 校验），但注释/文档须**诚实标注"非内容派生、不承载数据完整性"**。
4. 本阶段**不改** multipart 汇总的哈希算法（SHA1+base64 vs s3proxy 的 MD5-hex-dashN）——那需先核对 s3proxy `ETagByEtags` 真实实现，留到阶段 9。

---

## 改动 1：Backend CRC 默认关闭

### 文件：`backend/src/main.cpp`

```cpp
// 默认关闭 CRC 计算：纯吞吐压测态。需端到端校验时显式 --backend_compute_crc32c=true。
DEFINE_bool(backend_compute_crc32c, false,
            "compute CRC32C over received block bytes (also enables content etag)");
```

### 文件：`backend/src/backend_gds_sink.h`

```cpp
// compute_crc32c 默认 false：关闭时跳过字节扫描，etag 为占位符。
BackendGdsSink(std::string bind_host, int rdma_port,
               bool compute_crc32c = false);
```

> `rdma/ucx_sink.h` 的构造函数若也有该默认参数，同步改为 `false`。

---

## 改动 2：修占位 etag，关闭态下唯一且良构

### 文件：`backend/src/backend_block_data_plane_service.cpp`

当前 `BuildBlockEtag(crc)` 关闭态恒返回 `"block-0"`。改为**接收 object_id + block_size**，用它们拼唯一占位符；仅在 CRC 非 0（完整性态）时才用 crc 十六进制：

```cpp
// block etag：
//   - CRC 开启（完整性态）→ crc32c 的 8 位十六进制（内容派生，可校验）。
//   - CRC 关闭（压测态，默认）→ object_id + ":" + size 的占位符。
//     【注意】占位符仅保证各 block 唯一、供 part etag 汇总，不承载数据完整性。
[[nodiscard]] std::string BuildBlockEtag(std::uint32_t crc,
                                         const std::string& object_id,
                                         std::uint64_t block_size) {
  if (crc != 0) {
    char buf[9] = {};
    std::snprintf(buf, sizeof(buf), "%08x", crc);
    return std::string(buf);
  }
  return object_id + ":" + std::to_string(block_size);
}
```

调用点（GDS 与 UCX 两处）改为传入 `object_id` 与 `block_size`：

```cpp
response->set_etag(BuildBlockEtag(outcome.crc32c, object_id, src.block_size()));
```

> `object_id` 已由 `BuildBlockObjectId(upload_id, part_number, block_no)` 生成，天然含 `upload_id/part/block_no`，拼上 `block_size` 后每个 block 唯一。

---

## 改动 3（可选，本阶段可先留骨架）：完整性态的内容哈希

> 若暂不需要真实 s3proxy etag，本改动可跳过，仅保留改动 1/2。

当需要"backend 计算真实 part etag"时，让 sink 在 `compute_crc32c_` 开启时**除 CRC 外再算 block 字节的 MD5**（同一次扫描内完成，避免二次遍历），并把 MD5 十六进制作为 block etag。此时 `BuildBlockEtag` 的"CRC 开启"分支改用 MD5 而非 crc-hex。

留 `TODO(phase9)` 标记，等阶段 9 对齐 s3proxy 格式时统一实现。

---

## 验证

### 压测态（默认，CRC 关闭）
```bash
# backend 不带 --backend_compute_crc32c，即默认 false
./us3_turbo_backend --bind_host ... --rdma_port ...
# 跑分段上传 example
./us3_turbo_gds_multipart_example --proxy ... --num-parts 4 --part-size 5M
```
预期：
- [ ] 各 block etag 形如 `upload/p1/b0:4194304`，**互不相同**（不再是 `block-0`）。
- [ ] part etag 由不同 block etag 汇总，**不同 part 内容不同 → part etag 不同**。
- [ ] `CompleteMultipartUpload` 校验通过（client 回传的 part etag 与 proxy 侧一致）。
- [ ] 日志中 `crc={:x}` 恒为 0（CRC 未计算）。

### 完整性态（`--backend_compute_crc32c=true`）
- [ ] block etag 为 8 位十六进制 crc（或 MD5，若做了改动 3）。
- [ ] client `--verify-crc32c` 下 CRC 比对 MATCH。

---

## 后续（阶段 9，需先核对 s3proxy 源码）

- 核对 s3proxy `ETagByEtags` 真实算法（预期 `hex(md5(concat(各part二进制md5)))-<partcount>`）。
- 若与当前 `SHA1 + 4字节LE前缀 + base64` 不符，统一 `SessionManager::ComputeFinalETag` 与 `MultipartPutHandler::Aggregate` 两处汇总逻辑。
- 完整性态 block etag 切到 MD5，使 part/object etag 真正可被 s3proxy 读取端复核。
