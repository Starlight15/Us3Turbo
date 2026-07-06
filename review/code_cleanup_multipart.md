# Multipart 代码清理 — 消除冗余 + 串行化并发

依据 `code_review_multipart.md` 的评审结果，按"不抽象原则"（GDS/UCX 链路各自独立、避免相互影响）精选改动。

---

## 约束

1. **不抽象两条链路**：凡涉及 GDS/UCX 链路特定逻辑（source 填充、HandlePart、CallBackend）的重复代码**保持独立**，改一条不影响另一条。
2. **可抽纯算法/工具**：与链路无关的纯函数（ETag 汇总、时间戳、公共返回结构）可抽，无耦合风险。
3. 只改现有文件，不新建源文件（`utils.*` 已存在，追加函数）。

---

## 改动 1：抽 ETag 汇总算法（消除 A1 重复）

### 问题
`session_manager.cpp:138 ComputeFinalETag` 与 `multipart_put_handler.cpp:99 Aggregate` 里
"单 etag 直接返回 / 多 etag → 4 字节 LE count 前缀 + SHA1(拼接) + base64" 的逻辑**逐字节重复**两份。

### 文件：`proxy/src/common/utils.h`
追加声明：
```cpp
/** @brief 汇总多个 etag：单元素直接返回；多元素 → 4 字节 LE count + SHA1 拼接 + base64。 */
[[nodiscard]] std::string CombineETags(const std::vector<std::string>& etags);
```

### 文件：`proxy/src/common/utils.cpp`
追加实现：
```cpp
std::string CombineETags(const std::vector<std::string>& etags) {
  if (etags.empty()) return {};
  if (etags.size() == 1) return etags[0];

  std::string concatenated;
  for (const auto& e : etags) concatenated += e;
  const std::string sha1 = Sha1(concatenated);

  std::string raw(4 + sha1.size(), '\0');
  const std::uint32_t cnt = static_cast<std::uint32_t>(etags.size());
  raw[0] = static_cast<char>(cnt & 0xff);
  raw[1] = static_cast<char>((cnt >> 8) & 0xff);
  raw[2] = static_cast<char>((cnt >> 16) & 0xff);
  raw[3] = static_cast<char>((cnt >> 24) & 0xff);
  raw.replace(4, sha1.size(), sha1);

  return Base64Encode(raw);
}
```

### 文件：`proxy/src/multipart/session_manager.cpp`
替换 `ComputeFinalETag` 实现：
```cpp
std::string SessionManager::ComputeFinalETag(
    const std::vector<PartMetadata>& parts) {
  std::vector<std::string> etags;
  etags.reserve(parts.size());
  for (const auto& p : parts) etags.push_back(p.etag);
  return utils::CombineETags(etags);
}
```
> 注释改为"对齐 s3proxy ETagByEtags：单 part 直接用其 etag，多 part 汇总（见 utils::CombineETags）"。

### 文件：`proxy/src/multipart/multipart_put_handler.cpp`
`Aggregate` 里的 etag 汇总分支（line 122-143）替换为：
```cpp
// 2. 按 block_no 排序后汇总 etag。
auto sorted = results;
std::sort(sorted.begin(), sorted.end(),
          [](const auto& a, const auto& b) {
            return a.first.block_no < b.first.block_no;
          });
std::vector<std::string> etags;
etags.reserve(sorted.size());
for (const auto& [_, resp] : sorted) etags.push_back(resp.etag());

r.etag = utils::CombineETags(etags);
if (etags.size() == 1 && sorted[0].second.has_crc32c()) {
  r.crc32c = sorted[0].second.crc32c();
}
// 多 block 不汇总 crc（block crc 仅做传输校验，不合成 part crc）。
```

---

## 改动 2：合一 `CompletedMultipart` 结构（消除 A2 重复）

### 问题
`ProxyRpc::CompletedMultipart`（proxy_rpc.h:112-118）与 `Client::CompletedMultipart`（client.h:56-62）
字段全同，client 端手工逐字段拷贝。

### 文件：`client/include/us3_turbo/client/client.h`
删除 `Client::CompletedMultipart` 定义（line 56-62），改用别名：
```cpp
// ===== 分段上传接口 =====
using CompletedMultipart = ProxyRpc::CompletedMultipart;
```
> 放在 `class Client` 之前、`#include "client/src/rpc/proxy_rpc.h"` 已有的前提下。

### 文件：`client/src/client.cpp`
`CompleteMultipartUpload` 简化（line 345-357）：
```cpp
ProxyRpc::CompletedMultipart rpc_out;
if (!proxy_->CompleteMultipartUpload(request_id, upload_id, proto_parts,
                                     rpc_out)) {
  out = rpc_out;  // 失败时也拷贝 error
  return false;
}
out = std::move(rpc_out);
return out.ok;
```

---

## 改动 3：抽 `NowMs()`（消除 A4 重复）

### 文件：`proxy/src/common/utils.h`
追加：
```cpp
/** @brief 当前时间戳（毫秒，system_clock）。 */
[[nodiscard]] std::int64_t NowMs();
```

### 文件：`proxy/src/common/utils.cpp`
追加：
```cpp
std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
```

### 文件：`proxy/src/service/proxy_control_plane_service.cpp`
删除匿名 namespace 里的 `NowMs()`（line 24-28），改用 `utils::NowMs()`（line 284、PartMetadata 填充时）。

### 文件：`proxy/src/multipart/session_manager.cpp`
删除匿名 namespace 里的 `NowMs()`（line 14-18），改用 `utils::NowMs()`（三处：CreateSession、AddPart、CleanupExpired）。

---

## 改动 4：删冗余前向声明（C1）

### 文件：`proxy/src/service/proxy_control_plane_service.h`
删除 line 19：
```cpp
class MultipartPutHandler;  // ← 删除此行，已有 #include "multipart/multipart_put_handler.h"
```

---

## 改动 5：内联 `ForwardResult`（C2）

### 文件：`proxy/src/service/proxy_control_plane_service.cpp`
删除 `ForwardResult` 函数（line 85-88）。

两处调用（line 135、188）改为直接：
```cpp
response->CopyFrom(bresp);
```

---

## 改动 6：串行化 block 并发（D1）— GDS 链路

### 文件：`proxy/src/multipart/multipart_put_handler.cpp`

`HandleGdsPart` 改为串行调用（line 149-182）：
```cpp
MultipartPutHandler::PartResult MultipartPutHandler::HandleGdsPart(
    const std::string& request_id,
    const std::string& upload_id,
    std::uint32_t part_number,
    std::uint64_t part_size,
    const std::string& rdma_token) {
  if (backend_stub_ == nullptr) {
    return {false, "", "backend block stub not available", 0, 0};
  }
  const auto blocks = SplitToBlocks(part_size);
  spdlog::info("HandleGdsPart: req={} upload={} part={} size={} blocks={}",
               request_id, upload_id, part_number, part_size, blocks.size());

  // 串行调用各 block（block 数 ≤4，串行简单、无线程开销）。
  std::vector<std::pair<BlockPlan, ::us3_turbo::proxy::ProxyBackendPutBlockResponse>>
      results;
  results.reserve(blocks.size());
  for (const auto& block : blocks) {
    results.emplace_back(block,
        CallBackendPutBlockGds(request_id, upload_id, part_number,
                               block, rdma_token));
  }

  auto r = Aggregate(results, part_size);
  spdlog::info("HandleGdsPart done: req={} part={} ok={} etag={} crc={:x}",
               request_id, part_number, r.ok, r.etag, r.crc32c);
  return r;
}
```

删除头文件 `#include <future>`（line 4）。

---

## 改动 7：串行化 block 并发（D1）— UCX 链路

### 文件：`proxy/src/multipart/multipart_put_handler.cpp`

`HandleUcxPart` 改为串行调用（line 184-221）：
```cpp
MultipartPutHandler::PartResult MultipartPutHandler::HandleUcxPart(
    const std::string& request_id,
    const std::string& upload_id,
    std::uint32_t part_number,
    std::uint64_t part_size,
    std::uint64_t remote_addr,
    const std::string& packed_rkey,
    const std::string& client_ucx_addr) {
  if (backend_stub_ == nullptr) {
    return {false, "", "backend block stub not available", 0, 0};
  }
  const auto blocks = SplitToBlocks(part_size);
  spdlog::info("HandleUcxPart: req={} upload={} part={} size={} blocks={}",
               request_id, upload_id, part_number, part_size, blocks.size());

  // 串行调用各 block（block 数 ≤4，串行简单、无线程开销）。
  std::vector<std::pair<BlockPlan, ::us3_turbo::proxy::ProxyBackendPutBlockResponse>>
      results;
  results.reserve(blocks.size());
  for (const auto& block : blocks) {
    results.emplace_back(block,
        CallBackendPutBlockUcx(request_id, upload_id, part_number,
                               block, remote_addr, packed_rkey,
                               client_ucx_addr));
  }

  auto r = Aggregate(results, part_size);
  spdlog::info("HandleUcxPart done: req={} part={} ok={} etag={} crc={:x}",
               request_id, part_number, r.ok, r.etag, r.crc32c);
  return r;
}
```

> `#include <future>` 已在改动 6 删除。

---

## 验证

### ETag 汇总一致性
```bash
# 单 part → 直接用 part etag
# 多 part → 4 字节前缀 + SHA1 + base64
```
- [ ] SessionManager 与 MultipartPutHandler 生成的 etag 结构相同
- [ ] 单 block / 多 block part 的 etag 格式正确

### 串行调用正确性
```bash
./us3_turbo_gds_multipart_example --part-size 16M --num-parts 4
```
- [ ] 各 part 成功上传，etag 正确返回
- [ ] proxy 日志中各 part 的 block 按序调用（不再是并发 future）
- [ ] 吞吐无明显下降（block 数小，串行开销可忽略）

### 代码隔离性
- [ ] 修改 GDS 链路代码（HandleGdsPart / CallBackendPutBlockGds）**不影响** UCX 链路编译/运行
- [ ] 修改 UCX 链路代码（HandleUcxPart / CallBackendPutBlockUcx）**不影响** GDS 链路编译/运行
