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
  explicit SinglePut(UfileAcClient* client) : client_(client) {}

  [[nodiscard]] int PutGds(const ClientProxyPutRequest& request,
                           PutOutput& out);
  [[nodiscard]] int PutUcx(const ClientProxyPutRequest& request,
                           PutOutput& out);

 private:
  // 阶段①：参数校验
  [[nodiscard]] int ValidateGdsRequest(const ClientProxyPutRequest& req);
  [[nodiscard]] int ValidateUcxRequest(const ClientProxyPutRequest& req);

  // 阶段③：构造对象索引 fileidx + 填充输出（GDS/UCX 共用）
  void WriteObjectIndex(
      const std::string& request_id,
      const std::string& bucket, const std::string& key,
      const std::string& obj_id, std::uint64_t object_size,
      std::uint32_t crc32c, PutOutput& out);

  UfileAcClient* client_;
};

}  // namespace us3_turbo::proxy
