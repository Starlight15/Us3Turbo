#include "client/src/transport/ucx_get_channel.h"

#include <cassert>
#include <string>

#include <spdlog/spdlog.h>

#include "client/src/common/trace.h"
#include "client/src/rpc/proxy_rpc.h"
#include "us3_turbo/client/options.h"

namespace us3_turbo::client {

bool UcxGetChannel::StatObject(const std::string& bucket,
                               const std::string& key,
                               std::uint64_t& out_object_size,
                               std::string& out_error) const {
  const std::string req_id = detail::MakeReqId();
  return proxy_.StatObject(req_id, bucket, key, out_object_size, out_error);
}

bool UcxGetChannel::GetOnce(const std::string& bucket, const std::string& key,
                            MutableBufferView buffer,
                            GetPathResult& res) const {
  assert(ucx_mgr_ != nullptr);
  const std::string req_id = detail::MakeReqId();

  // AcquireDescriptor 注册 host buffer 并打包 rkey（方向无关，PUT/GET 共用）。
  UcxMemoryManager::Descriptor desc;
  if (!ucx_mgr_->AcquireDescriptor(buffer.data, buffer.size, desc)) {
    spdlog::error("UcxGet (req={}): AcquireDescriptor failed", req_id);
    return false;
  }
  UcxDataSource ucx_source{desc.remote_addr, desc.rkey, desc.client_ucx_addr};

  return proxy_.UcxGet(req_id, bucket, key, buffer.size, ucx_source, res);
}

}  // namespace us3_turbo::client
