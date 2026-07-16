#include "client/src/transport/gds_get_channel.h"

#include <cassert>
#include <string>

#include <spdlog/spdlog.h>

#include "client/src/common/trace.h"
#include "client/src/rpc/proxy_rpc.h"
#include "us3_turbo/client/options.h"

#include <cuobjclient.h>

namespace us3_turbo::client {

bool GdsGetChannel::StatObject(const std::string& bucket,
                               const std::string& key,
                               std::uint64_t& out_object_size,
                               std::string& out_error) const {
  const std::string request_id = detail::MakeRequestId();
  return proxy_.StatObject(request_id, bucket, key, out_object_size, out_error);
}

bool GdsGetChannel::GetOnce(const std::string& bucket, const std::string& key,
                            MutableBufferView buffer,
                            GetPathResult& result) const {
  assert(gds_mgr_ != nullptr);
  const std::string request_id = detail::MakeRequestId();

  GdsMemoryManager::Token token;
  if (!gds_mgr_->AcquireToken(buffer.data, buffer.size, 0, token, CUOBJ_GET)) {
    spdlog::error("GdsGet (req={}): AcquireToken(CUOBJ_GET) failed",
                  request_id);
    return false;
  }
  GdsDataSource gds_source{std::string(token.str())};

  return proxy_.GdsGet(request_id, bucket, key, buffer.size, gds_source,
                       result);
}

}  // namespace us3_turbo::client
