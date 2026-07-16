#pragma once

#include <cstdint>
#include <string>

#include "control_plane.pb.h"
#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

class IUploadIndex;
class UfileAcClient;

/* 单步上传输出 */
struct PutOutput {
  std::string etag;
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
};

/* 单步上传逻辑层 */
class SinglePut {
 public:
  explicit SinglePut(IUploadIndex* index, UfileAcClient* client)
      : index_(index), client_(client) {}

  /* GDS 单步上传，写数据 + 写索引 */
  [[nodiscard]] int PutGds(const ClientProxyPutRequest& request,
                           PutOutput& out);
  /* UCX 单步上传，写数据 + 写索引 */
  [[nodiscard]] int PutUcx(const ClientProxyPutRequest& request,
                           PutOutput& out);

 private:
  /* 校验 GDS 上传请求合法性 */
  [[nodiscard]] int ValidateGdsRequest(const ClientProxyPutRequest& req);
  /* 校验 UCX 上传请求合法性 */
  [[nodiscard]] int ValidateUcxRequest(const ClientProxyPutRequest& req);

  /* 写对象索引并填充输出 */
  [[nodiscard]] bool WriteObjectIndex(const std::string& request_id,
                                      const std::string& bucket,
                                      const std::string& key,
                                      const std::string& obj_id,
                                      std::uint64_t object_size,
                                      std::uint32_t crc32c, PutOutput& out);

  IUploadIndex* index_;
  UfileAcClient* client_;
};

}  // namespace us3_turbo::proxy
