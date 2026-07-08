#pragma once

// block_storage.h — 阶段三：BlockStorage 简化为 UfileAcClient 的轻量透传包装器。
//
// 阶段一/二迁移后 multipart 数据面已走 ufile-ac 自定义协议（Multipart 直接经
// UfileAcClient 循环写 block）。本类原有的 brpc 数据面实现（PutPartGds/Ucx、
// SplitToBlocks、Aggregate、CallBackendPutBlock*、BackendDataPlane_Stub）已无调用，
// 阶段三全部删除。保留此类仅为避免改动 Multipart 构造签名（main 仍传
// BlockStorage* 给 Multipart）；如需进一步简化可让 Multipart 直接持 UfileAcClient*。
//
// 注意：proxy 控制面仍用 brpc（ProxyService brpc server），故 CMake 仍链接
// us3_turbo_brpc；此处清理的是"数据面 backend brpc channel"，非控制面。

#include <cstdint>

namespace us3_turbo::proxy {

class UfileAcClient;  // 前向声明，定义见 ufile_ac_client.h

// 轻量透传壳：暴露 UfileAcClient 给 Multipart 写 block。
class BlockStorage {
 public:
  explicit BlockStorage(UfileAcClient* client) : client_(client) {}

  [[nodiscard]] UfileAcClient* GetUfileAcClient() const { return client_; }

 private:
  UfileAcClient* client_;
};

}  // namespace us3_turbo::proxy
