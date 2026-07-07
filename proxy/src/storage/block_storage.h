#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <brpc/channel.h>

#include "control_plane.pb.h"
#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

// block 切分存储层（迁自 multipart_put_handler）：自持 POOLED channel +
// BackendDataPlane_Stub，把单个 part 按 block_size 切分串行调 backend PutBlock。
// GDS/UCX 各独立方法。串行 block（block 数 ≤4）、Aggregate 汇总 etag 不变。
class BlockStorage {
 public:
  BlockStorage(const std::string& backend_endpoint, int timeout_ms,
               std::uint64_t block_size = 4ULL * 1024 * 1024);

  // ret_code: 0=成功，非 0=PROXY_ERR_*；error 为失败时的上下文（成功时为空）。
  // PartResult 保持结构体返回（需同时回带 etag + bytes + crc）。
  struct PartResult {
    int           ret_code{0};
    std::string   etag;
    std::string   error;
    std::uint32_t crc32c{0};     // 单 block 时取该 block crc；多 block 暂不汇总
    std::uint64_t bytes_written{0};
  };

  /** @brief GDS 路径：part → blocks 串行上传 → 汇总。 */
  PartResult PutPartGds(const std::string& request_id,
                        const std::string& upload_id,
                        std::uint32_t part_number,
                        std::uint64_t part_size,
                        const std::string& rdma_token);

  /** @brief UCX 路径：part → blocks 串行上传 → 汇总。 */
  PartResult PutPartUcx(const std::string& request_id,
                        const std::string& upload_id,
                        std::uint32_t part_number,
                        std::uint64_t part_size,
                        std::uint64_t remote_addr,
                        const std::string& packed_rkey,
                        const std::string& client_ucx_addr);

 private:
  struct BlockPlan {
    std::uint32_t block_no;
    std::uint64_t source_offset;
    std::uint64_t block_size;
  };

  std::vector<BlockPlan> SplitToBlocks(std::uint64_t part_size) const;

  ::us3_turbo::proxy::ProxyBackendPutBlockResponse CallBackendPutBlockGds(
      const std::string& request_id,
      const std::string& upload_id,
      std::uint32_t part_number,
      const BlockPlan& block,
      const std::string& rdma_token);

  ::us3_turbo::proxy::ProxyBackendPutBlockResponse CallBackendPutBlockUcx(
      const std::string& request_id,
      const std::string& upload_id,
      std::uint32_t part_number,
      const BlockPlan& block,
      std::uint64_t remote_addr,
      const std::string& packed_rkey,
      const std::string& client_ucx_addr);

  // 汇总 block 响应：全 ok 才算成功；part etag = 单 block → 该 block etag，
  // 多 block → 4 字节 LE count 前缀 + SHA1(各 etag 拼接) 再 base64。
  PartResult Aggregate(
      const std::vector<std::pair<BlockPlan,
                                  ::us3_turbo::proxy::ProxyBackendPutBlockResponse>>& results,
      std::uint64_t part_size);

  int                                     timeout_ms_;
  std::uint64_t                           block_size_;
  std::shared_ptr<brpc::Channel>          channel_;
  std::unique_ptr<::us3_turbo::proxy::BackendDataPlane_Stub> stub_;
};

}  // namespace us3_turbo::proxy
