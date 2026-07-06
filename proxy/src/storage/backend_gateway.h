#pragma once

#include <memory>
#include <string>

#include <brpc/channel.h>

#include "control_plane.pb.h"
#include "proxy/src/service/single_put_service.h"  // PutResult

namespace us3_turbo::proxy {

// 单步转发存储层：自持 SINGLE channel + Control_Stub，执行 backend RPC。
// GDS/UCX 各独立方法；参数校验留在 SinglePutService，本类只管转发。
class BackendGateway {
 public:
  BackendGateway(const std::string& backend_endpoint, int timeout_ms);

  [[nodiscard]] PutResult ForwardGdsPut(
      const ::us3_turbo::proxy::ClientProxyPutRequest& request);
  [[nodiscard]] PutResult ForwardUcxPut(
      const ::us3_turbo::proxy::ClientProxyPutRequest& request);

 private:
  int                                     timeout_ms_;
  std::shared_ptr<brpc::Channel>          channel_;
  std::unique_ptr<::us3_turbo::proxy::Control_Stub> stub_;
};

}  // namespace us3_turbo::proxy
