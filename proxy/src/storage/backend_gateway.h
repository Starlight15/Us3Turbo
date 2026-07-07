#pragma once

#include <memory>
#include <string>

#include <brpc/channel.h>

#include "control_plane.pb.h"
#include "proxy/src/common/errors.h"
#include "proxy/src/service/single_put.h"  // PutOutput

namespace us3_turbo::proxy {

// 单步转发存储层：自持 SINGLE channel + Control_Stub，执行 backend RPC。
// GDS/UCX 各独立方法；参数校验留在 SinglePut，本类只管转发。
// 成功返回 0，失败先 spdlog 再 return 错误码。
class BackendGateway {
 public:
  BackendGateway(const std::string& backend_endpoint, int timeout_ms);

  [[nodiscard]] int ForwardGdsPut(
      const ::us3_turbo::proxy::ClientProxyPutRequest& request,
      PutOutput& out);
  [[nodiscard]] int ForwardUcxPut(
      const ::us3_turbo::proxy::ClientProxyPutRequest& request,
      PutOutput& out);

 private:
  int                                     timeout_ms_;
  std::shared_ptr<brpc::Channel>          channel_;
  std::unique_ptr<::us3_turbo::proxy::Control_Stub> stub_;
};

}  // namespace us3_turbo::proxy
