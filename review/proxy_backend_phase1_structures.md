# Proxy ↔ Backend 交互结构 - 阶段1：定义数据结构

## 🎯 目标

创建 Proxy 和 Backend 交互的核心数据结构，不影响现有代码。

---

## 📁 新建文件：`proxy/src/contracts/set_put_request.h`

```cpp
#pragma once
#include <cstdint>
#include <string>

namespace us3_turbo::proxy {

/**
 * @brief Backend 从 Client 拉取数据的模式。
 */
enum class SetPullMode : uint8_t {
  kGdsToken = 1,        // Backend 用 GDS token 从 GPU 显存拉取
  kUcxRemoteMemory = 2, // Backend 用 UCX 从 host 内存拉取
};

/**
 * @brief Backend 从 client 拉取数据的源描述符。
 * 
 * 根据 mode 字段选择对应的字段组：
 * - kGdsToken: 使用 gds_rdma_token
 * - kUcxRemoteMemory: 使用 remote_addr / packed_rkey / client_ucx_addr
 * 
 * source_offset：当前 block 在 client buffer 中的起始偏移（单位：字节）。
 * 例如：10MB 对象分 2 个 5MB 块，Block 1 的 source_offset = 5MB。
 */
struct SetPullSource {
  /**
   * @brief 构造 GDS 拉取源。
   * @param rdma_token CUDA GDS RDMA token
   * @param offset 当前 block 在 client buffer 中的起始偏移
   */
  static SetPullSource Gds(std::string rdma_token, uint64_t offset) {
    SetPullSource s;
    s.mode = SetPullMode::kGdsToken;
    s.gds_rdma_token = std::move(rdma_token);
    s.source_offset = offset;
    return s;
  }

  /**
   * @brief 构造 UCX 拉取源。
   * @param addr UCX remote memory 地址
   * @param rkey UCX packed rkey
   * @param ucx_addr Client UCX 地址（格式："ip:port"）
   * @param offset 当前 block 在 client buffer 中的起始偏移
   */
  static SetPullSource Ucx(uint64_t addr, std::string rkey, 
                          std::string ucx_addr, uint64_t offset) {
    SetPullSource s;
    s.mode = SetPullMode::kUcxRemoteMemory;
    s.remote_addr = addr;
    s.packed_rkey = std::move(rkey);
    s.client_ucx_addr = std::move(ucx_addr);
    s.source_offset = offset;
    return s;
  }

  SetPullMode mode{SetPullMode::kGdsToken};

  // GDS 专用字段
  std::string gds_rdma_token;

  // UCX 专用字段
  uint64_t remote_addr{0};
  std::string packed_rkey;
  std::string client_ucx_addr;  // "ip:port"

  // 公共字段：当前 block 在 client buffer 中的起始偏移
  uint64_t source_offset{0};
};

/**
 * @brief Proxy 向 Backend 发起的单块上传请求。
 * 
 * Proxy 收到 client 的对象上传请求后，拆分为多个 block 请求，每个 block：
 * 1. 填充对象元数据（object_id / object_offset）
 * 2. 填充块元数据（block_id / block_no / block_size / last_piece）
 * 3. 填充存储策略（set_id / replica / ogn / algorithm）
 * 4. 填充数据源描述符（source：GDS 或 UCX）
 * 5. 填充校验信息（expected_crc32c）
 * 
 * Backend 根据 source 拉取数据，写入存储系统，返回实际写入的字节数和 CRC。
 */
struct ProxySetPutBlockRequest {
  std::string request_id;  // 全链路追踪 ID（格式：client_req_id + "_block" + block_no）

  // ========== 存储策略 ==========
  uint64_t set_id{0};      // Proxy 选中的 set 编号（存储系统的 set）

  // ========== 对象元数据 ==========
  std::string object_id;   // Proxy 生成的对象 ID（最终写入索引）
  uint64_t object_offset{0};  // 当前 block 在对象中的偏移（用于对象组装）

  // ========== 块元数据 ==========
  std::string block_id;    // 块 ID（格式：object_id + "_" + block_no）
  uint32_t block_no{0};    // 块序号（从 0 开始）
  uint64_t block_size{0};  // 当前块大小（单位：字节）
  bool last_piece{false};  // 是否为对象的最后一块

  // ========== 存储协议参数 ==========
  uint32_t replica{3};     // 副本数（存储系统要求）
  uint32_t ogn{2};         // 原始数据块组数（存储系统纠删码参数）
  uint32_t algorithm{2};   // 编码算法（存储系统纠删码参数）

  // ========== 数据源 ==========
  SetPullSource source;    // Backend 从哪里拉取数据（GDS 或 UCX）

  // ========== 端到端校验 ==========
  uint32_t expected_crc32c{0};  // Client 端计算的 CRC（包含 zero_padding）
};

/**
 * @brief Backend 返回的单块上传结果。
 * 
 * Backend 完成数据拉取和写入后，返回：
 * 1. 是否成功（ok）
 * 2. 错误信息（error_code / error_message）
 * 3. 实际写入字节数（bytes_written）
 * 4. Backend 计算的 CRC（crc32c）
 * 5. 对齐填充的零字节数（zero_padding）
 */
struct ProxySetPutBlockResponse {
  bool ok{false};
  int32_t error_code{0};
  std::string error_message;

  std::string block_id;         // 回显 block_id（便于 proxy 匹配请求）
  uint64_t bytes_written{0};    // Backend 实际写入字节数（含 zero_padding）
  uint32_t crc32c{0};           // Backend 计算的 CRC（含 zero_padding）
  
  /**
   * @brief Backend 对齐块大小后填充的零字节数。
   * 
   * 存储系统可能要求块大小对齐（如 4KB），Backend 会在原始数据后填充零字节。
   * Proxy 用此字段验证：CRC32C(原始数据 + zero_padding) == crc32c。
   */
  uint32_t zero_padding{0};
};

}  // namespace us3_turbo::proxy
```

---

## ✅ 验证步骤

1. **编译测试**：
   ```bash
   # 确保头文件可以被正确包含
   echo '#include "proxy/src/contracts/set_put_request.h"' > /tmp/test.cpp
   g++ -std=c++17 -I. -c /tmp/test.cpp -o /tmp/test.o
   ```

2. **工厂函数测试**：
   ```cpp
   // 测试代码片段（不需要实际运行）
   auto gds_src = SetPullSource::Gds("token_abc", 0);
   auto ucx_src = SetPullSource::Ucx(0x7f000000, "rkey_xyz", "192.168.1.100:12345", 5242880);
   ```

---

## 📝 注意事项

1. ✅ 命名空间使用 `us3_turbo::proxy`
2. ✅ 所有字段都有详细注释
3. ✅ 工厂函数避免手动填充错误
4. ✅ 与现有代码完全独立，不影响编译
