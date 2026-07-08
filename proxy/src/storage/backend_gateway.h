#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <brpc/channel.h>

#include "control_plane.pb.h"
#include "proxy/src/common/errors.h"
#include "proxy/src/service/single_put.h"  // PutOutput

namespace us3_turbo::proxy {

// 单步转发存储层：自持 N 条 SINGLE channel + Control_Stub 组成连接池，
// 请求轮询分配到各 channel，解除单连接吞吐瓶颈。GDS/UCX 各独立方法；
// 参数校验留在 SinglePut，本类只管转发。成功返回 0，失败先 LOG_* 再 return
// 错误码。rid 从 request.request_id() 取。
//
// 并发：brpc channel 自身线程安全，多 bthread 同调同一 stub 不串包，故轮询
// 无需加锁；next_idx_ 用 atomic 保证递增无竞态。
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
  int timeout_ms_;
  std::vector<std::shared_ptr<brpc::Channel>>                     channels_;
  std::vector<std::unique_ptr<::us3_turbo::proxy::Control_Stub>>  stubs_;
  std::atomic<std::uint64_t> next_idx_{0};  // 轮询计数器（无锁）
};

}  // namespace us3_turbo::proxy
