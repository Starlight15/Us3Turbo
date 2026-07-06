#pragma once

#include <cstdint>
#include <string>

#include "control_plane.pb.h"
#include "proxy/src/common/status.h"

namespace us3_turbo::proxy {

class BackendGateway;  // 前向声明，定义在 storage/backend_gateway.h

// 单步上传服务层结果：status 携带域错误码，etag/crc32c/bytes_written 回填接口层。
struct PutResult {
  ProxyStatus   status;
  std::string   etag;
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
};

// 单步上传服务：参数校验后委托存储层转发。GDS/UCX 各自独立方法，
// 不共享内部分支函数。
class SinglePutService {
 public:
  // gateway 由 main 持有，本类不拥有。
  explicit SinglePutService(BackendGateway* gateway);

  [[nodiscard]] PutResult PutGds(const ::us3_turbo::proxy::ClientProxyPutRequest& request);
  [[nodiscard]] PutResult PutUcx(const ::us3_turbo::proxy::ClientProxyPutRequest& request);

 private:
  BackendGateway* gateway_;
};

}  // namespace us3_turbo::proxy
