#pragma once

#include <cstddef>

#include "client/src/common/request.h"
#include "client/src/memory_manager/ucx_memory_manager.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

class ProxyRpc;

/** @brief UCX 链路的 GetChannel:host 内存走 ucp_mem_map + packed rkey,
 * backend ucp_put_nbx 反向推数据。与 UcxPutChannel 平级、职责相反。 */
class UcxGetChannel final {
 public:
  UcxGetChannel(const ClientOptions& options, const ProxyRpc& proxy, UcxMemoryManager* ucx_mgr)
      : options_(options), proxy_(proxy), ucx_mgr_(ucx_mgr) {}

  /** @brief 查对象布局(object_size)，调用方据此分配 buffer。 */
  [[nodiscard]] bool StatObject(const std::string& bucket, const std::string& key,
                                std::uint64_t& out_object_size, std::string& out_error) const;

  /** @brief 单次 GET 尝试：buffer 须已按 StatObject 返回的 size 分配。 */
  [[nodiscard]] bool GetOnce(const std::string& bucket, const std::string& key,
                             MutableBufferView buffer, GetPathResult& result) const;

 private:
  const ClientOptions& options_;
  const ProxyRpc& proxy_;
  UcxMemoryManager* ucx_mgr_;
};

}  // namespace us3_turbo::client
