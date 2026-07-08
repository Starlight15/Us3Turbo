#pragma once

#include <cstdint>
#include <string>

#include "control_plane.pb.h"
#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

class BackendGateway;  // 前向声明，定义在 storage/backend_gateway.h

// 单步上传输出：成功时由服务层填充，接口层据此回填 PutPathResult。
// 定义在此供 service 与 storage 共用（BackendGateway::Forward*Put 复用）。
struct PutOutput {
  std::string   etag;
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
};

// 单步上传服务：参数校验后委托存储层转发。GDS/UCX 各自独立方法，
// 不共享内部分支函数。成功返回 0，失败先 spdlog 再 return 错误码。
class SinglePut {
 public:
  // gateway 由 main 持有，本类不拥有。
  explicit SinglePut(BackendGateway* gateway);

  [[nodiscard]] int PutGds(const ClientProxyPutRequest& request,
                           PutOutput& out);
  [[nodiscard]] int PutUcx(const ClientProxyPutRequest& request,
                           PutOutput& out);

 private:
  BackendGateway* gateway_;
};

}  // namespace us3_turbo::proxy
