#pragma once

// multipart_put_handler.h — proxy 把单个 part 按 block_size（默认 4MB）切分，
// 并发调 backend BackendDataPlane.PutBlock。
//
// GDS 路径：block 带 source_offset（相对 part token 注册 region），backend
// 据此从 client 显存子区间反向 RDMA-READ。
// UCX 路径：proxy 把 remote_addr + source_offset 算好传给 backend，backend
// ucp_get_nbx 直接用该地址拉取。
//
// 并发：同一 part 的多个 block 用 std::launch::async 并发调用 backend；
// 任意 block 失败则该 part 失败。结果按 block_no 排序后汇总成 part etag。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "control_plane.pb.h"

namespace us3_turbo::proxy {

class MultipartPutHandler {
 public:
  /**
   * @param backend_stub  到 backend 的 block 级 stub（BackendDataPlane_Stub）。
   *                      生命周期由 ProxyControlPlaneService 拥有，需先于本对象析构。
   * @param timeout_ms    单次 PutBlock RPC 超时。
   * @param block_size    切分块大小，默认 4MB（对齐 s3proxy BlkSize）。
   */
  MultipartPutHandler(::us3_turbo::proxy::BackendDataPlane_Stub* backend_stub,
                      int timeout_ms,
                      std::uint64_t block_size = 4ULL * 1024 * 1024);

  struct PartResult {
    bool          ok{false};
    std::string   etag;
    std::string   error;
    std::uint32_t crc32c{0};     // 单 block 时取该 block crc；多 block 暂不汇总
    std::uint64_t bytes_written{0};
  };

  /** @brief GDS 路径：part → blocks 并发上传 → 汇总。 */
  PartResult HandleGdsPart(const std::string& request_id,
                           const std::string& upload_id,
                           std::uint32_t part_number,
                           std::uint64_t part_size,
                           const std::string& rdma_token);

  /** @brief UCX 路径：part → blocks 并发上传 → 汇总。 */
  PartResult HandleUcxPart(const std::string& request_id,
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

  ::us3_turbo::proxy::BackendDataPlane_Stub* backend_stub_;
  int                                       timeout_ms_;
  std::uint64_t                             block_size_;
};

}  // namespace us3_turbo::proxy
