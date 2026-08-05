#include "client/src/transport/gds_get_channel.h"

#include <cassert>
#include <string>

#include <spdlog/spdlog.h>

#include "client/src/common/trace.h"
#include "client/src/rpc/proxy_rpc.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/common/logger.h"

#include <cuobjclient.h>

namespace us3_turbo::client {

bool GdsGetChannel::StatObject(const std::string& bucket, const std::string& key,
                               std::uint64_t& out_object_size, std::string& out_trace_id,
                               std::string& out_error) const {
  const std::string req_id = detail::MakeReqId();
  return proxy_.StatObject(req_id, bucket, key, out_object_size, out_trace_id, out_error);
}

bool GdsGetChannel::GetOnce(const std::string& bucket, const std::string& key,
                            std::string_view trace_id,
                            MutableBufferView buffer, GetPathResult& res) const {
  assert(gds_mgr_ != nullptr);
  const std::string req_id = detail::MakeReqId();

  GdsMemoryManager::Token token;
  if (!gds_mgr_->AcquireToken(buffer.data, buffer.size, token, CUOBJ_GET)) {
    LOG_ERROR(req_id, "AcquireToken(CUOBJ_GET) failed");
    return false;
  }
  return proxy_.GdsGet(req_id, trace_id, bucket, key, buffer.size, std::string(token.str()), res);
}

}  // namespace us3_turbo::client
