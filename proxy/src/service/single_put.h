#pragma once

#include <cstdint>
#include <string>

#include "control_plane.pb.h"
#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

class UfileAcClient;  // 前向声明，定义在 storage/ufile_ac_client.h

// 单步上传输出：由 SinglePut 从 UfileAcClient 的 BlockResult 转填，接口层据此回填 PutPathResult。
struct PutOutput {
  std::string   etag;
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
};

// 单步上传服务：参数校验后委托存储层转发。GDS/UCX 各自独立方法，
// 不共享内部分支函数。成功返回 0，失败先 spdlog 再 return 错误码。
class SinglePut {
 public:
  // client 由 main 持有，本类不拥有。
  explicit SinglePut(UfileAcClient* client);

  [[nodiscard]] int PutGds(const ClientProxyPutRequest& request,
                           PutOutput& out);
  [[nodiscard]] int PutUcx(const ClientProxyPutRequest& request,
                           PutOutput& out);

 private:
  UfileAcClient* client_;
};

}  // namespace us3_turbo::proxy
