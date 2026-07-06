#pragma once

// backend_block_data_plane_service.h — block 级数据面服务（proxy → backend）。
//
// 与 BackendDataPlaneService（实现 Control：GdsPut/UcxPut 单步路径）平行、独立。
// 本类实现 proto service BackendDataPlane 的 PutBlock：proxy 分段上传时把 part
// 切成 4MB block 后并发调本 RPC。backend 按 GdsBlockSource.source_offset 或
// UcxBlockSource.remote_addr（已加偏移）从 client 反向拉取 block 字节，算
// CRC32C/ETag 后丢弃（v1 不写盘）。
//
// 复用现有 BackendGdsSink / UcxSink 的 ReceiveAndDiscard（已加 source_offset
// 形参）。block etag = BuildEtag(crc32c)（与单步 GdsPut/UcxPut 的 etag 一致），
// 避免为 block 数据另开 SHA1 路径而需保留字节。crc32c 仅在 sink 启用计算时非 0。

#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "backend/src/backend_gds_sink.h"
#include "backend/src/rdma/ucx_sink.h"
#include "control_plane.pb.h"

namespace us3_turbo::backend {

class BackendBlockDataPlaneService final
    : public ::us3_turbo::proxy::BackendDataPlane {
 public:
  BackendBlockDataPlaneService(BackendGdsSink& sink,
                               rdma::UcxSink& ucx_sink);

  void PutBlock(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo::proxy::ProxyBackendPutBlockRequest* request,
      ::us3_turbo::proxy::ProxyBackendPutBlockResponse* response,
      google::protobuf::Closure* done) override;

 private:
  BackendGdsSink& sink_;
  rdma::UcxSink&  ucx_sink_;
};

}  // namespace us3_turbo::backend
