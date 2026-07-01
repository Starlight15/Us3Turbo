# Proxy ↔ Backend 交互结构 - 阶段4：实现 Proxy 分块逻辑

## 🎯 目标

实现 Proxy 端的分块逻辑：将 Client 的对象上传请求拆分为多个 block，并发调用 Backend 的 PutBlock RPC。

---

## 📁 新建文件：`proxy/src/put_handler.h`

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "client/src/contracts/put_request.h"
#include "proxy/src/backend_rpc.h"
#include "proxy/src/contracts/set_put_request.h"

namespace us3_turbo::proxy {

/**
 * @brief Proxy 端处理 Client PUT 请求的核心逻辑。
 * 
 * 职责：
 * 1. 选择 set（根据 bucket/key 哈希或负载均衡策略）
 * 2. 生成 object_id（UUID 或其他唯一标识）
 * 3. 将对象拆分为多个 block
 * 4. 并发调用 Backend PutBlock RPC
 * 5. 汇总结果返回给 Client
 */
class PutHandler {
 public:
  /**
   * @param backend_endpoints Backend 节点列表（格式：["ip1:port1", "ip2:port2", ...]）
   * @param block_size 分块大小（默认 5MB）
   */
  PutHandler(const std::vector<std::string>& backend_endpoints,
             uint64_t block_size = 5 * 1024 * 1024);

  ~PutHandler();

  /**
   * @brief 处理 GDS 路径的对象上传。
   * 
   * @param request Client 请求（bucket/key/object_size/gds_source）
   * @param result 返回上传结果（etag/crc32c/bytes_written）
   * @return true 成功，false 失败
   */
  [[nodiscard]] bool HandleGdsPut(
      const us3_turbo::client::ClientProxyPutRequest& request,
      us3_turbo::client::PutPathResult& result);

  /**
   * @brief 处理 UCX 路径的对象上传。
   * 
   * @param request Client 请求（bucket/key/object_size/ucx_source）
   * @param result 返回上传结果（etag/crc32c/bytes_written）
   * @return true 成功，false 失败
   */
  [[nodiscard]] bool HandleUcxPut(
      const us3_turbo::client::ClientProxyPutRequest& request,
      us3_turbo::client::PutPathResult& result);

 private:
  uint64_t block_size_;
  std::vector<std::unique_ptr<BackendRpc>> backends_;

  // 根据 bucket/key 选择 set（简单哈希）
  [[nodiscard]] uint64_t SelectSet(const std::string& bucket,
                                   const std::string& key) const;

  // 生成唯一的 object_id
  [[nodiscard]] std::string GenerateObjectId() const;

  // 拆分对象为多个 block 请求（GDS 路径）
  [[nodiscard]] std::vector<ProxySetPutBlockRequest> SplitToBlocksGds(
      const us3_turbo::client::ClientProxyPutRequest& request,
      uint64_t set_id,
      const std::string& object_id) const;

  // 拆分对象为多个 block 请求（UCX 路径）
  [[nodiscard]] std::vector<ProxySetPutBlockRequest> SplitToBlocksUcx(
      const us3_turbo::client::ClientProxyPutRequest& request,
      uint64_t set_id,
      const std::string& object_id) const;

  // 并发执行所有 block 请求
  [[nodiscard]] bool ExecuteBlocks(
      const std::vector<ProxySetPutBlockRequest>& block_requests,
      std::vector<ProxySetPutBlockResponse>& block_responses);

  // 汇总所有 block 的结果
  [[nodiscard]] bool AggregateResults(
      const std::vector<ProxySetPutBlockResponse>& block_responses,
      us3_turbo::client::PutPathResult& result) const;
};

}  // namespace us3_turbo::proxy
```

---

## 📁 新建文件：`proxy/src/put_handler.cpp`

```cpp
#include "proxy/src/put_handler.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <numeric>

#include <spdlog/spdlog.h>

namespace us3_turbo::proxy {

PutHandler::PutHandler(const std::vector<std::string>& backend_endpoints,
                       uint64_t block_size)
    : block_size_(block_size) {
  for (const auto& endpoint : backend_endpoints) {
    backends_.emplace_back(std::make_unique<BackendRpc>(
        endpoint, std::chrono::milliseconds(30000)));
  }
  spdlog::info("PutHandler: initialized with {} backends, block_size={}",
               backends_.size(), block_size_);
}

PutHandler::~PutHandler() = default;

bool PutHandler::HandleGdsPut(
    const us3_turbo::client::ClientProxyPutRequest& request,
    us3_turbo::client::PutPathResult& result) {
  const auto set_id = SelectSet(request.bucket, request.key);
  const auto object_id = GenerateObjectId();

  spdlog::info("HandleGdsPut: req={}, bucket=, key={}, size={}, set_id={}, object_id={}",
               request.request_id, request.bucket, request.key, 
               request.object_size, set_id, object_id);

  // 拆分为多个 block
  auto block_requests = SplitToBlocksGds(request, set_id, object_id);
  std::vector<ProxySetPutBlockResponse> block_responses(block_requests.size());

  // 并发执行所有 block
  if (!ExecuteBlocks(block_requests, block_responses)) {
    spdlog::error("HandleGdsPut: req={}, failed to execute blocks", request.request_id);
    return false;
  }

  // 汇总结果
  if (!AggregateResults(block_responses, result)) {
    spdlog::error("HandleGdsPut: req={}, failed to aggregate results", request.request_id);
    return false;
  }

  result.ok = true;
  spdlog::info("HandleGdsPut: req={}, success, etag={}, crc32c=0x{:08x}, bytes={}",
               request.request_id, result.etag, result.crc32c, result.bytes_written);
  return true;
}

bool PutHandler::HandleUcxPut(
    const us3_turbo::client::ClientProxyPutRequest& request,
    us3_turbo::client::PutPathResult& result) {
  const auto set_id = SelectSet(request.bucket, request.key);
  const auto object_id = GenerateObjectId();

  spdlog::info("HandleUcxPut: req={}, bucket={}, key={}, size={}, set_id={}, object_id={}",
               request.request_id, request.bucket, request.key,
               request.object_size, set_id, object_id);

  // 拆分为多个 block
  auto block_requests = SplitToBlocksUcx(request, set_id, object_id);
  std::vector<ProxySetPutBlockResponse> block_responses(block_requests.size());

  // 并发执行所有 block
  if (!ExecuteBlocks(block_requests, block_responses)) {
    spdlog::error("HandleUcxPut: req={}, failed to execute blocks", request.request_id);
    return false;
  }

  // 汇总结果
  if (!AggregateResults(block_responses, result)) {
    spdlog::error("HandleUcxPut: req={}, failed to aggregate results", request.request_id);
    return false;
  }

  result.ok = true;
  spdlog::info("HandleUcxPut: req={}, success, etag={}, crc32c=0x{:08x}, bytes={}",
               request.request_id, result.etag, result.crc32c, result.bytes_written);
  return true;
}

uint64_t PutHandler::SelectSet(const std::string& bucket,
                               const std::string& key) const {
  // 简单哈希：bucket + key → set_id
  std::hash<std::string> hasher;
  return hasher(bucket + key) % 100;  // 假设有 100 个 set
}

std::string PutHandler::GenerateObjectId() const {
  // 简单实现：时间戳 + 随机数
  auto now = std::chrono::system_clock::now().time_since_epoch().count();
  return "obj_" + std::to_string(now);
}

std::vector<ProxySetPutBlockRequest> PutHandler::SplitToBlocksGds(
    const us3_turbo::client::ClientProxyPutRequest& request,
    uint64_t set_id,
    const std::string& object_id) const {
  std::vector<ProxySetPutBlockRequest> blocks;

  const uint64_t num_blocks = (request.object_size + block_size_ - 1) / block_size_;
  for (uint64_t i = 0; i < num_blocks; ++i) {
    ProxySetPutBlockRequest block;
    block.request_id = request.request_id + "_block" + std::to_string(i) + "_gds";
    block.set_id = set_id;
    block.object_id = object_id;
    block.object_offset = i * block_size_;
    block.block_id = object_id + "_" + std::to_string(i);
    block.block_no = i;
    block.block_size = std::min(block_size_, request.object_size - i * block_size_);
    block.last_piece = (i == num_blocks - 1);
    block.replica = 3;
    block.ogn = 2;
    block.algorithm = 2;
    block.source = SetPullSource::Gds(request.gds_source->rdma_token, i * block_size_);
    block.expected_crc32c = 0;  // TODO: Client 需要传递每个 block 的 CRC

    blocks.push_back(std::move(block));
  }

  return blocks;
}

std::vector<ProxySetPutBlockRequest> PutHandler::SplitToBlocksUcx(
    const us3_turbo::client::ClientProxyPutRequest& request,
    uint64_t set_id,
    const std::string& object_id) const {
  std::vector<ProxySetPutBlockRequest> blocks;

  const uint64_t num_blocks = (request.object_size + block_size_ - 1) / block_size_;
  for (uint64_t i = 0; i < num_blocks; ++i) {
    ProxySetPutBlockRequest block;
    block.request_id = request.request_id + "_block" + std::to_string(i) + "_ucx";
    block.set_id = set_id;
    block.object_id = object_id;
    block.object_offset = i * block_size_;
    block.block_id = object_id + "_" + std::to_string(i);
    block.block_no = i;
    block.block_size = std::min(block_size_, request.object_size - i * block_size_);
    block.last_piece = (i == num_blocks - 1);
    block.replica = 3;
    block.ogn = 2;
    block.algorithm = 2;
    block.source = SetPullSource::Ucx(
        request.ucx_source->remote_addr,
        request.ucx_source->packed_rkey,
        request.ucx_source->client_ucx_addr,
        i * block_size_);
    block.expected_crc32c = 0;  // TODO: Client 需要传递每个 block 的 CRC

    blocks.push_back(std::move(block));
  }

  return blocks;
}

bool PutHandler::ExecuteBlocks(
    const std::vector<ProxySetPutBlockRequest>& block_requests,
    std::vector<ProxySetPutBlockResponse>& block_responses) {
  // 并发执行所有 block（使用 std::async）
  std::vector<std::future<bool>> futures;

  for (size_t i = 0; i < block_requests.size(); ++i) {
    futures.push_back(std::async(std::launch::async, [this, i, &block_requests, &block_responses]() {
      // 简单轮询选择 backend
      auto& backend = backends_[i % backends_.size()];
      return backend->PutBlock(block_requests[i], block_responses[i]);
    }));
  }

  // 等待所有 block 完成
  bool all_ok = true;
  for (auto& fut : futures) {
    if (!fut.get()) {
      all_ok = false;
    }
  }

  return all_ok;
}

bool PutHandler::AggregateResults(
    const std::vector<ProxySetPutBlockResponse>& block_responses,
    us3_turbo::client::PutPathResult& result) const {
  // 汇总所有 block 的结果
  result.bytes_written = 0;
  for (const auto& resp : block_responses) {
    if (!resp.ok) {
      result.error_code = resp.error_code;
      result.error_message = resp.error_message;
      return false;
    }
    result.bytes_written += resp.bytes_written;
  }

  // 生成 etag（简单实现：最后一个 block 的 crc32c）
  result.etag = "etag_" + std::to_string(block_responses.back().crc32c);
  
  // 生成对象级别的 CRC（简单实现：所有 block CRC 的 XOR）
  result.crc32c = 0;
  for (const auto& resp : block_responses) {
    result.crc32c ^= resp.crc32c;
  }

  return true;
}

}  // namespace us3_turbo::proxy
```

---

## ✅ 验证步骤

1. **编译通过**：
   ```bash
   g++ -std=c++17 -I. -I<brpc_include> -pthread \
     -c proxy/src/put_handler.cpp -o proxy/src/put_handler.o
   ```

2. **逻辑检查**：
   - ✅ 正确计算 `num_blocks`
   - ✅ 每个 block 的 `source_offset` 递增
   - ✅ 最后一个 block 设置 `last_piece = true`
   - ✅ 并发执行所有 block（`std::async`）
   - ✅ 汇总结果（bytes_written 求和）

3. **TODO 项**：
   - ⚠️ `expected_crc32c` 目前为 0，需要 Client 计算每个 block 的 CRC 传给 Proxy
   - ⚠️ 对象级别 CRC 的计算方式需要与 Client 一致（当前用 XOR 是简化实现）

---

## 📝 注意事项

1. ✅ GDS 和 UCX 的拆分逻辑完全独立（两个方法）
2. ✅ 并发执行 block 请求，提高吞吐量
3. ✅ 日志包含 `request_id`，便于全链路追踪
4. ⚠️ 当前 set 选择和 object_id 生成是简化实现，生产环境需要更健壮的策略
5. ⚠️ 当前 backend 选择用轮询（`i % backends_.size()`），可改为负载均衡策略
