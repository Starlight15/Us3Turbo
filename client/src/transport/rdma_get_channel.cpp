#include "client/src/transport/rdma_get_channel.h"

#include <cassert>
#include <string>

#include <spdlog/spdlog.h>

#include "client/src/common/trace.h"
#include "client/src/rpc/proxy_rpc.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/common/logger.h"

namespace us3_turbo::client {

bool RdmaGetChannel::StatObject(const std::string& bucket, const std::string& key,
                                std::uint64_t& out_object_size, std::string& out_error) const {
  const std::string req_id = detail::MakeReqId();
  return proxy_.StatObject(req_id, bucket, key, out_object_size, out_error);
}

bool RdmaGetChannel::GetOnce(const std::string& bucket, const std::string& key,
                             MutableBufferView buffer, GetPathResult& res) const {
  assert(rdma_mgr_ != nullptr);
  const std::string req_id = detail::MakeReqId();

  // For GET, backend RDMA WRITEs to client buffer, so register MR with WRITE access.
  RdmaMemoryManager::Descriptor desc;
  if (!rdma_mgr_->AcquireDescriptorForWrite(buffer.data, buffer.size, desc)) {
    LOG_ERROR(req_id, "AcquireDescriptorForWrite failed");
    return false;
  }
  return proxy_.RdmaGet(req_id, bucket, key, buffer.size, desc.token, res);
}

}  // namespace us3_turbo::client
