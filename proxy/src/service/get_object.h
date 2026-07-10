#pragma once

#include <cstdint>
#include <string>

#include "control_plane.pb.h"
#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

class IUploadIndex;
class UfileAcClient;

/**
 * @brief StatObject 输出：object 布局，供 client 分配 buffer。
 */
struct StatObjectOutput {
  std::uint64_t object_size{0};
  std::uint64_t block_size{0};
  std::string   hash;
};

/**
 * @brief GET 输出：按块读取 + crc 重组校验后的结果。
 */
struct GetOutput {
  std::uint32_t crc32c{0};
  std::uint64_t bytes_read{0};
  std::string   hash;
};

/**
 * @brief 读取逻辑层（GDS）。镜像 SinglePut 的结构：校验 → 编排 backend 调用 →
 * 填输出。与 UCX GET（未来）完全独立实现。
 */
class GetObject {
 public:
  explicit GetObject(IUploadIndex* index, UfileAcClient* client)
      : index_(index), client_(client) {}

  [[nodiscard]] int StatObject(const StatObjectRequest& request,
                               StatObjectOutput& out);

  [[nodiscard]] int GetGds(const ClientProxyGetRequest& request,
                           GetOutput& out);

 private:
  [[nodiscard]] int ValidateGdsRequest(const ClientProxyGetRequest& req);

  IUploadIndex*  index_;
  UfileAcClient* client_;
};

}  // namespace us3_turbo::proxy
