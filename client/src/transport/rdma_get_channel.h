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

  /** @brief 查对象布局(object_size)，调用方据此分配 buffer。 */
  [[nodiscard]] bool StatObject(const std::string& bucket, const std::string& key,
                                std::uint64_t& out_object_size, std::string& out_error) const;

  /** @brief 单次 GET 尝试：buffer 须已按 StatObject 返回的 size 分配。 */
  [[nodiscard]] bool GetOnce(const std::string& bucket, const std::string& key,
                             MutableBufferView buffer, GetPathResult& res) const;

 private:
  const ClientOptions& opts_;
  const ProxyRpc& proxy_;
  RdmaMemoryManager* rdma_mgr_;
};

}  // namespace us3_turbo::client
