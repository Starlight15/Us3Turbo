#pragma once

#include <cstdint>
#include <string>

#include "control_plane.pb.h"
#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

class UfileAcClient;

/**
 * @brief 单步上传输出
 */
struct PutOutput {
  std::string   etag;
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
};

/**
 * @brief 单步上传逻辑层
 */
class SinglePut {
 public:
  explicit SinglePut(UfileAcClient* client);

  explicit SinglePut(UfileAcClient* client) : client_(client) {}

  [[nodiscard]] int PutGds(const ClientProxyPutRequest& request,
                           PutOutput& out);
  [[nodiscard]] int PutUcx(const ClientProxyPutRequest& request,
                           PutOutput& out);

 private:
  UfileAcClient* client_;
};

}  // namespace us3_turbo::proxy
