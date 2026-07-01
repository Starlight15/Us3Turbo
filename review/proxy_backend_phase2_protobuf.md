# Proxy ↔ Backend 交互结构 - 阶段2：定义 Protobuf

## 🎯 目标

定义 Proxy 和 Backend 之间的 RPC 接口，适配阶段1的 C++ 结构体。

---

## 📁 修改文件：`proto/backend_service.proto`（新建或追加）

```protobuf
syntax = "proto3";

package us3_turbo.backend;

/**
 * Backend 从 Client 拉取数据的模式
 */
enum SetPullMode {
  PULL_MODE_UNSPECIFIED = 0;      // 无效值（protobuf 要求）
  PULL_MODE_GDS_TOKEN = 1;        // GDS token 拉取
  PULL_MODE_UCX_REMOTE_MEMORY = 2; // UCX remote memory 拉取
}

/**
 * Backend 从 client 拉取数据的源描述符
 */
message SetPullSource {
  SetPullMode mode = 1;
  
  // GDS 专用字段
  string gds_rdma_token = 2;
  
  // UCX 专用字段
  uint64 remote_addr = 3;
  string packed_rkey = 4;
  string client_ucx_addr = 5;
  
  // 公共字段：当前 block 在 client buffer 中的起始偏移
  uint64 source_offset = 6;
}

/**
 * Proxy → Backend 的单块上传请求
 */
message ProxySetPutBlockRequest {
  string request_id = 1;
  
  // 存储策略
  uint64 set_id = 2;
  
  // 对象元数据
  string object_id = 3;
  uint64 object_offset = 4;
  
  // 块元数据
  string block_id = 5;
  uint32 block_no = 6;
  uint64 block_size = 7;
  bool last_piece = 8;
  
  // 存储协议参数
  uint32 replica = 9;
  uint32 ogn = 10;
  uint32 algorithm = 11;
  
  // 数据源
  SetPullSource source = 12;
  
  // 端到端校验
  uint32 expected_crc32c = 13;
}

/**
 * Backend → Proxy 的单块上传响应
 */
message ProxySetPutBlockResponse {
  bool ok = 1;
  int32 error_code = 2;
  string error_message = 3;
  
  string block_id = 4;
  uint64 bytes_written = 5;
  uint32 crc32c = 6;
  uint32 zero_padding = 7;
}

/**
 * Backend 服务定义
 */
service BackendService {
  /**
   * 上传单个块（Backend 主动拉取数据）
   */
  rpc PutBlock(ProxySetPutBlockRequest) returns (ProxySetPutBlockResponse);
}
```

---

## 🔧 编译 Protobuf

```bash
# 假设 proto 文件在 proto/ 目录
protoc --cpp_out=. --grpc_out=. --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
  proto/backend_service.proto

# 或者使用 brpc 的编译方式
protoc --cpp_out=. proto/backend_service.proto
```

---

## 📝 C++ ↔ Protobuf 转换工具（可选）

### 新建文件：`proxy/src/contracts/set_put_converter.h`

```cpp
#pragma once
#include "proxy/src/contracts/set_put_request.h"
#include "backend_service.pb.h"

namespace us3_turbo::proxy {

/**
 * @brief 将 C++ 结构体转换为 protobuf 消息
 */
inline void ToProto(const SetPullSource& src, 
                   us3_turbo::backend::SetPullSource* proto) {
  proto->set_mode(static_cast<us3_turbo::backend::SetPullMode>(src.mode));
  proto->set_gds_rdma_token(src.gds_rdma_token);
  proto->set_remote_addr(src.remote_addr);
  proto->set_packed_rkey(src.packed_rkey);
  proto->set_client_ucx_addr(src.client_ucx_addr);
  proto->set_source_offset(src.source_offset);
}

inline void ToProto(const ProxySetPutBlockRequest& req,
                   us3_turbo::backend::ProxySetPutBlockRequest* proto) {
  proto->set_request_id(req.request_id);
  proto->set_set_id(req.set_id);
  proto->set_object_id(req.object_id);
  proto->set_object_offset(req.object_offset);
  proto->set_block_id(req.block_id);
  proto->set_block_no(req.block_no);
  proto->set_block_size(req.block_size);
  proto->set_last_piece(req.last_piece);
  proto->set_replica(req.replica);
  proto->set_ogn(req.ogn);
  proto->set_algorithm(req.algorithm);
  ToProto(req.source, proto->mutable_source());
  proto->set_expected_crc32c(req.expected_crc32c);
}

/**
 * @brief 将 protobuf 消息转换为 C++ 结构体
 */
inline void FromProto(const us3_turbo::backend::ProxySetPutBlockResponse& proto,
                     ProxySetPutBlockResponse& resp) {
  resp.ok = proto.ok();
  resp.error_code = proto.error_code();
  resp.error_message = proto.error_message();
  resp.block_id = proto.block_id();
  resp.bytes_written = proto.bytes_written();
  resp.crc32c = proto.crc32c();
  resp.zero_padding = proto.zero_padding();
}

}  // namespace us3_turbo::proxy
```

---

## ✅ 验证步骤

1. **protobuf 编译通过**：
   ```bash
   protoc --cpp_out=. proto/backend_service.proto
   # 检查生成的 backend_service.pb.h 和 backend_service.pb.cc
   ```

2. **转换工具编译通过**：
   ```bash
   g++ -std=c++17 -I. -I<protobuf_include> \
     -c proxy/src/contracts/set_put_converter.h
   ```

3. **字段对齐检查**：
   - ✅ C++ 的 `SetPullMode::kGdsToken = 1` 对应 protobuf 的 `PULL_MODE_GDS_TOKEN = 1`
   - ✅ 所有字段名一致（驼峰命名 vs 下划线命名自动转换）

---

## 📝 注意事项

1. ✅ protobuf enum 的第 0 值必须是 `UNSPECIFIED`（protobuf3 要求）
2. ✅ 转换函数处理了所有字段，无遗漏
3. ✅ 命名空间隔离清晰（`us3_turbo::backend` vs `us3_turbo::proxy`）
