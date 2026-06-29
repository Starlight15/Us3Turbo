#include "backend/src/rdma/ucx_resources.h"

#include <cstdlib>

namespace us3_turbo::backend::rdma {

EpGuard::~EpGuard() {
  if (ep_ == nullptr) return;
  ucp_request_param_t param{};
  void* req = ucp_ep_close_nbx(ep_, &param);
  if (req != nullptr && !UCS_PTR_IS_ERR(req)) {
    while (ucp_request_check_status(req) == UCS_INPROGRESS) {
      ucp_worker_progress(worker_);
    }
    ucp_request_free(req);
  }
}

RkeyGuard::~RkeyGuard() {
  if (rkey_ != nullptr) ucp_rkey_destroy(rkey_);
}

HostBuffer::HostBuffer(std::size_t size) : size_(size) {
  if (size > 0) data_ = std::malloc(size);
}
HostBuffer::~HostBuffer() {
  if (data_ != nullptr) std::free(data_);
}

}  // namespace us3_turbo::backend::rdma
