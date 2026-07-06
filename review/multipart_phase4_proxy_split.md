# 阶段 4：实现 Proxy 分段切分与并发上传逻辑

## 目标

在 Proxy 端实现将 client 提供的 part 按固定 block_size（4MB）切分为多个 block，并发调用 Backend 的 `PutBlock` RPC。

---

## 约束

1. **固定块大小**：默认 4MB（可配置），与 s3proxy 的 `BlkSize` 对齐
2. **GDS/UCX 独立**：切分逻辑分别实现在 `SplitToBlocksGds` 和 `SplitToBlocksUcx`
3. **并发上传**：使用 `std::async` 或线程池对同一 part 的多个 block 并发调用 Backend
4. **CRC/ETag 汇总**：所有 block 的 CRC32C 拼接后重新计算该 part 的最终 CRC 和 ETag
5. **依赖阶段 1**：使用 `ProxyBackendPutBlockRequest`/`Response` proto

---

## 核心数据结构

### 文件：`proxy/src/multipart/multipart_put_handler.h`

```cpp
#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "control_plane.pb.h"

namespace us3_turbo::proxy {

// 单个 block 的请求信息
struct BlockRequest {
  uint32_t block_no;        // 该 part 内的 block 序号（从 0 开始）
  uint64_t source_offset;   // 相对 part 起始的偏移
  uint64_t block_size;      // 本 block 大小
};

// 单个 block 的上传结果
struct BlockResult {
  uint32_t block_no;
  bool ok;
  std::string etag;        // Backend 返回的 block etag
  std::string error_message;
};

class MultipartPutHandler {
 public:
  explicit MultipartPutHandler(uint64_t block_size = 4 * 1024 * 1024);  // 默认 4MB
  
  // GDS 路径：part → blocks，并发上传到 Backend
  bool HandleGdsPart(
      const std::string& request_id,
      const std::string& upload_id,
      uint32_t part_number,
      uint64_t part_size,
      const std::string& rdma_token,
      std::string& out_etag,
      std::string& out_error);
  
  // UCX 路径：part → blocks，并发上传到 Backend
  bool HandleUcxPart(
      const std::string& request_id,
      const std::string& upload_id,
      uint32_t part_number,
      uint64_t part_size,
      uint64_t remote_addr,
      const std::string& packed_rkey,
      const std::string& client_ucx_addr,
      std::string& out_etag,
      std::string& out_error);
  
 private:
  uint64_t block_size_;
  
  // 切分 part 为多个 block
  std::vector<BlockRequest> SplitToBlocksGds(uint64_t part_size);
  std::vector<BlockRequest> SplitToBlocksUcx(uint64_t part_size);
  
  // 并发执行所有 block 的上传
  std::vector<BlockResult> ExecuteBlocksGds(
      const std::string& request_id,
      const std::string& upload_id,
      uint32_t part_number,
      const std::string& rdma_token,
      const std::vector<BlockRequest>& blocks);
  
  std::vector<BlockResult> ExecuteBlocksUcx(
      const std::string& request_id,
      const std::string& upload_id,
      uint32_t part_number,
      uint64_t remote_addr,
      const std::string& packed_rkey,
      const std::string& client_ucx_addr,
      const std::vector<BlockRequest>& blocks);
  
  // 汇总所有 block 的结果，计算 part 级 ETag
  bool AggregateResults(
      const std::vector<BlockResult>& results,
      std::string& out_etag,
      std::string& out_error);
  
  // 调用 Backend RPC（单个 block）
  BlockResult CallBackendPutBlockGds(
      const std::string& request_id,
      const std::string& upload_id,
      uint32_t part_number,
      const BlockRequest& block,
      const std::string& rdma_token);
  
  BlockResult CallBackendPutBlockUcx(
      const std::string& request_id,
      const std::string& upload_id,
      uint32_t part_number,
      const BlockRequest& block,
      uint64_t remote_addr,
      const std::string& packed_rkey,
      const std::string& client_ucx_addr);
};

}  // namespace
```

---

## 实现逻辑

### 文件：`proxy/src/multipart/multipart_put_handler.cpp`

#### 1. 切分逻辑（GDS 和 UCX 对称）

```cpp
std::vector<BlockRequest> MultipartPutHandler::SplitToBlocksGds(uint64_t part_size) {
  std::vector<BlockRequest> blocks;
  
  uint64_t offset = 0;
  uint32_t block_no = 0;
  
  while (offset < part_size) {
    uint64_t remaining = part_size - offset;
    uint64_t current_block_size = std::min(remaining, block_size_);
    
    BlockRequest req;
    req.block_no = block_no++;
    req.source_offset = offset;
    req.block_size = current_block_size;
    
    blocks.push_back(req);
    offset += current_block_size;
  }
  
  return blocks;
}

std::vector<BlockRequest> MultipartPutHandler::SplitToBlocksUcx(uint64_t part_size) {
  // 逻辑完全相同，保持 GDS/UCX 独立
  return SplitToBlocksGds(part_size);
}
```

#### 2. 并发执行（GDS 路径为例）

```cpp
std::vector<BlockResult> MultipartPutHandler::ExecuteBlocksGds(
    const std::string& request_id,
    const std::string& upload_id,
    uint32_t part_number,
    const std::string& rdma_token,
    const std::vector<BlockRequest>& blocks) {
  
  std::vector<std::future<BlockResult>> futures;
  
  // 启动所有 block 的异步上传
  for (const auto& block : blocks) {
    futures.push_back(std::async(std::launch::async, [&, block]() {
      return CallBackendPutBlockGds(request_id, upload_id, part_number, block, rdma_token);
    }));
  }
  
  // 等待所有结果
  std::vector<BlockResult> results;
  for (auto& fut : futures) {
    results.push_back(fut.get());
  }
  
  return results;
}
```

**UCX 路径：**

```cpp
std::vector<BlockResult> MultipartPutHandler::ExecuteBlocksUcx(
    const std::string& request_id,
    const std::string& upload_id,
    uint32_t part_number,
    uint64_t remote_addr,
    const std::string& packed_rkey,
    const std::string& client_ucx_addr,
    const std::vector<BlockRequest>& blocks) {
  
  std::vector<std::future<BlockResult>> futures;
  
  for (const auto& block : blocks) {
    futures.push_back(std::async(std::launch::async, [&, block]() {
      return CallBackendPutBlockUcx(
          request_id, upload_id, part_number, block,
          remote_addr, packed_rkey, client_ucx_addr);
    }));
  }
  
  std::vector<BlockResult> results;
  for (auto& fut : futures) {
    results.push_back(fut.get());
  }
  
  return results;
}
```

#### 3. Backend RPC 调用（GDS 路径为例）

```cpp
BlockResult MultipartPutHandler::CallBackendPutBlockGds(
    const std::string& request_id,
    const std::string& upload_id,
    uint32_t part_number,
    const BlockRequest& block,
    const std::string& rdma_token) {
  
  BlockResult result;
  result.block_no = block.block_no;
  
  // 构造 proto 请求
  ProxyBackendPutBlockRequest req;
  req.set_request_id(request_id);
  req.set_upload_id(upload_id);
  req.set_part_number(part_number);
  req.set_block_no(block.block_no);
  
  auto* gds_src = req.mutable_gds_source();
  gds_src->set_rdma_token(rdma_token);
  gds_src->set_source_offset(block.source_offset);  // 关键：透传 offset
  gds_src->set_block_size(block.block_size);
  
  // 调用 Backend RPC（假设已有 backend_stub_）
  ProxyBackendPutBlockResponse resp;
  brpc::Controller cntl;
  
  backend_stub_->PutBlock(&cntl, &req, &resp, nullptr);
  
  if (cntl.Failed()) {
    result.ok = false;
    result.error_message = cntl.ErrorText();
    return result;
  }
  
  result.ok = resp.ok();
  result.crc32c = resp.crc32c();
  result.error_message = resp.error_message();
  
  return result;
}
```

**UCX 路径关键差异：**

```cpp
BlockResult MultipartPutHandler::CallBackendPutBlockUcx(
    const std::string& request_id,
    const std::string& upload_id,
    uint32_t part_number,
    const BlockRequest& block,
    uint64_t remote_addr,
    const std::string& packed_rkey,
    const std::string& client_ucx_addr) {
  
  BlockResult result;
  result.block_no = block.block_no;
  
  ProxyBackendPutBlockRequest req;
  req.set_request_id(request_id);
  req.set_upload_id(upload_id);
  req.set_part_number(part_number);
  req.set_block_no(block.block_no);
  
  auto* ucx_src = req.mutable_ucx_source();
  ucx_src->set_remote_addr(remote_addr + block.source_offset);  // 地址加偏移
  ucx_src->set_packed_rkey(packed_rkey);
  ucx_src->set_client_ucx_addr(client_ucx_addr);
  ucx_src->set_block_size(block.block_size);
  
  ProxyBackendPutBlockResponse resp;
  brpc::Controller cntl;
  
  backend_stub_->PutBlock(&cntl, &req, &resp, nullptr);
  
  // ... 处理响应，同 GDS 路径
  
  return result;
}
```

#### 4. 结果汇总

```cpp
bool MultipartPutHandler::AggregateResults(
    const std::vector<BlockResult>& results,
    std::string& out_etag,
    std::string& out_error) {
  
  // 1. 检查是否有失败的 block
  for (const auto& r : results) {
    if (!r.ok) {
      out_error = "block " + std::to_string(r.block_no) + " failed: " + r.error_message;
      return false;
    }
  }
  
  // 2. 按 block_no 排序（保证顺序）
  std::vector<BlockResult> sorted = results;
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.block_no < b.block_no; });
  
  // 3. 计算 part 级 ETag（参考 s3proxy 的 ETagByEtags）
  //    收集所有 block 的 etag
  std::vector<std::string> block_etags;
  for (const auto& r : sorted) {
    block_etags.push_back(r.etag);
  }
  
  // 单 block → 直接用该 block 的 etag
  // 多 block → SHA1(所有 block etag 拼接) 的 base64，前缀 block count
  if (block_etags.size() == 1) {
    out_etag = block_etags[0];
  } else {
    std::string concatenated;
    for (const auto& etag : block_etags) {
      concatenated += etag;
    }
    std::string sha1 = utils::SHA1(concatenated);
    
    // 对齐 s3proxy：前缀 4 字节的 block count（little endian）
    std::vector<uint8_t> result(4);
    uint32_t blk_count = block_etags.size();
    result[0] = blk_count & 0xff;
    result[1] = (blk_count >> 8) & 0xff;
    result[2] = (blk_count >> 16) & 0xff;
    result[3] = (blk_count >> 24) & 0xff;
    result.insert(result.end(), sha1.begin(), sha1.end());
    
    out_etag = utils::Base64Encode(std::string(result.begin(), result.end()));
  }
  
  return true;
}
```

#### 5. HandleGdsPart 主流程

```cpp
bool MultipartPutHandler::HandleGdsPart(
    const std::string& request_id,
    const std::string& upload_id,
    uint32_t part_number,
    uint64_t part_size,
    const std::string& rdma_token,
    std::string& out_etag,
    std::string& out_error) {
  
  // 1. 切分为 blocks
  auto blocks = SplitToBlocksGds(part_size);
  
  LOG(INFO) << "SplitToBlocksGds: upload_id=" << upload_id
            << ", part_number=" << part_number
            << ", part_size=" << part_size
            << ", block_count=" << blocks.size();
  
  // 2. 并发上传所有 blocks
  auto results = ExecuteBlocksGds(request_id, upload_id, part_number, rdma_token, blocks);
  
  // 3. 汇总结果
  bool ok = AggregateResults(results, out_etag, out_error);
  
  if (ok) {
    LOG(INFO) << "HandleGdsPart success: upload_id=" << upload_id
              << ", part_number=" << part_number
              << ", etag=" << out_etag;
  } else {
    LOG(ERROR) << "HandleGdsPart failed: upload_id=" << upload_id
               << ", part_number=" << part_number
               << ", error=" << out_error;
  }
  
  return ok;
}
```

**HandleUcxPart 流程对称，调用 `SplitToBlocksUcx` 和 `ExecuteBlocksUcx`。**

---

## 集成到 Proxy RPC 服务

### 文件：`proxy/src/service/proxy_control_plane_service.cpp`

修改阶段 3 的 `UploadPartGds` 实现：

```cpp
void ProxyControlPlaneService::UploadPartGds(
    brpc::Controller* cntl,
    const UploadPartGdsRequest* request,
    UploadPartResponse* response,
    google::protobuf::Closure* done) {
  
  brpc::ClosureGuard done_guard(done);
  
  // 1. 校验会话（同阶段 3）
  UploadSession* session = session_manager_->GetSession(request->upload_id());
  if (!session) {
    response->set_ok(false);
    response->set_error_message("upload_id not found");
    return;
  }
  
  if (session->path != PATH_GDS) {
    response->set_ok(false);
    response->set_error_message("session path is not PATH_GDS");
    return;
  }
  
  // 2. 调用 MultipartPutHandler 处理
  std::string etag, error;
  
  bool ok = multipart_put_handler_->HandleGdsPart(
      request->request_id(),
      request->upload_id(),
      request->part_number(),
      request->part_size(),
      request->rdma_token(),
      etag,
      error);
  
  if (!ok) {
    response->set_ok(false);
    response->set_error_message(error);
    return;
  }
  
  // 3. 添加 part 元数据到会话
  PartMetadata part;
  part.part_number = request->part_number();
  part.part_size = request->part_size();
  part.etag = etag;
  part.upload_time_ms = GetCurrentTimeMillis();
  
  session_manager_->AddPart(request->upload_id(), part);
  
  // 4. 返回响应
  response->set_ok(true);
  response->set_etag(etag);
  response->set_bytes_written(request->part_size());
  // 注意：不设置 crc32c，因为默认关闭
}
```

**在构造函数中初始化 handler：**

```cpp
ProxyControlPlaneService::ProxyControlPlaneService()
    : session_manager_(std::make_unique<SessionManager>()),
      multipart_put_handler_(std::make_unique<MultipartPutHandler>(4 * 1024 * 1024)) {
  // ...
}
```

---

## 编译验证

```bash
# 1. 编译 MultipartPutHandler
cd proxy
g++ -std=c++17 -I../proto -I../generated \
    -c src/multipart/multipart_put_handler.cpp \
    -o build/multipart_put_handler.o

# 2. 链接到 Proxy 服务
g++ build/proxy_service.o build/session_manager.o build/multipart_put_handler.o \
    ../generated/control_plane.pb.o -lbrpc -lprotobuf -lpthread \
    -o build/proxy_server
```

---

## 单元测试用例

```cpp
// 文件：proxy/test/test_multipart_put_handler.cpp

TEST(MultipartPutHandler, SplitToBlocks_20MB) {
  MultipartPutHandler handler(4 * 1024 * 1024);  // 4MB block
  
  auto blocks = handler.SplitToBlocksGds(20 * 1024 * 1024);  // 20MB part
  
  ASSERT_EQ(blocks.size(), 5);  // 20MB / 4MB = 5 blocks
  EXPECT_EQ(blocks[0].source_offset, 0);
  EXPECT_EQ(blocks[0].block_size, 4 * 1024 * 1024);
  EXPECT_EQ(blocks[4].source_offset, 16 * 1024 * 1024);
  EXPECT_EQ(blocks[4].block_size, 4 * 1024 * 1024);
}

TEST(MultipartPutHandler, SplitToBlocks_3MB) {
  MultipartPutHandler handler(4 * 1024 * 1024);
  
  auto blocks = handler.SplitToBlocksGds(3 * 1024 * 1024);  // 3MB part < block_size
  
  ASSERT_EQ(blocks.size(), 1);  // 只有 1 个 block
  EXPECT_EQ(blocks[0].block_size, 3 * 1024 * 1024);
}
```

---

## 验收标准

- [ ] 20MB part 切分为 5 个 4MB block（最后一个可能 < 4MB）
- [ ] 3MB part 切分为 1 个 3MB block
- [ ] `ExecuteBlocksGds` 并发调用 Backend，所有 block 都返回 ok=true
- [ ] `AggregateResults` 能检测任意 block 失败并返回错误
- [ ] GDS 路径的 `source_offset` 正确透传到 Backend（0, 4M, 8M, ...）
- [ ] UCX 路径的 `remote_addr` 正确加偏移（base_addr, base_addr+4M, ...）

---

## 后续阶段依赖

- **阶段 5**：Backend PutBlock 实现（接收本阶段发来的带 offset 的请求）
- **阶段 6**：Client 端分段注册与调用（完整端到端测试）
