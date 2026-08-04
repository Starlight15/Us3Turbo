#pragma once

#include <cstdint>
#include <string>

#include "control_plane.pb.h"
#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

class IUploadIndex;
class UfileAcClient;

/* StatObject 输出：object 布局，供 client 分配 buffer。 */
struct StatObjectOutput {
  std::uint64_t object_size{0};
  std::uint64_t block_size{0};
  std::string hash;
};

/* GET 输出：按块读取 + crc 重组校验后的结果。 */
struct GetOutput {
  std::uint32_t crc32c{0};
  std::uint64_t bytes_read{0};
  std::string hash;
};

/* 读取逻辑层（GDS）。镜像 SinglePut 的结构：校验 → 编排 backend 调用 →
   如果同时需要两个通路请分别调用。 */
class GetObject {
 public:
  explicit GetObject(IUploadIndex* index, UfileAcClient* client) : index_(index), client_(client) {}

  /* 查询 object 布局信息，返回 size/block_size/hash 供 client 分配 buffer。 */
  [[nodiscard]] int StatObject(const StatObjectRequest& request, StatObjectOutput& out);

  /* GDS 路径 GET：按块 RDMA 读取 + crc32c 重组校验。 */
  [[nodiscard]] int GetGds(const ClientProxyGetRequest& request, GetOutput& out);

  /* RDMA (libibverbs) 路径 GET：按块从 backend 读 + backend RDMA WRITE 到 client buffer +
   * crc32c 重组校验。 */
  [[nodiscard]] int GetRdma(const ClientProxyGetRequest& request, GetOutput& out);


 private:
  /* 校验 GDS GET 请求合法性。 */
  [[nodiscard]] int ValidateGdsRequest(const ClientProxyGetRequest& req);

  /* 校验 RDMA GET 请求合法性。 */
  [[nodiscard]] int ValidateRdmaRequest(const ClientProxyGetRequest& req);


  IUploadIndex* index_;
  UfileAcClient* client_;
};

}  // namespace us3_turbo::proxy
