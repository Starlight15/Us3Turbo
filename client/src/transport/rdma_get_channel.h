#pragma once

#include <cstddef>

#include "client/src/common/request.h"
#include "client/src/memory_manager/rdma_memory_manager.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

class ProxyRpc;

/** @brief RDMA (libibverbs) 链路的 GetChannel: host 内存走 libibverbs RDMA WRITE +
 * backend 从 NVMe 读数据后 RDMA WRITE 推到 client host buffer。与 GdsGetChannel 平级。 */
class RdmaGetChannel final {
 public:
  RdmaGetChannel(const ClientOptions& options, const ProxyRpc& proxy, RdmaMemoryManager* rdma_mgr)
      : opts_(options), proxy_(proxy), rdma_mgr_(rdma_mgr) {}

  /** @brief 单次 GET 尝试：buffer 须已按 StatObject 返回的 size 分配。
   * trace_id 由 StatObject 返回、调用方透传。 */
  [[nodiscard]] bool GetOnce(const std::string& bucket, const std::string& key,
                             std::string_view trace_id,
                             MutableBufferView buffer, GetPathResult& res) const;

 private:
  const ClientOptions& opts_;
  const ProxyRpc& proxy_;
  RdmaMemoryManager* rdma_mgr_;
};

}  // namespace us3_turbo::client
