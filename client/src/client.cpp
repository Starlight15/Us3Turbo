#include "us3_turbo/client/client.h"

#include <chrono>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

#include "client/src/common/retry_policy.h"
#include "client/src/contracts/put_request.h"
#include "client/src/gds_transport/gds_memory_manager.h"
#include "client/src/proxy_rpc.h"
#include "client/src/rdma_transport/ucx_memory_manager.h"
#include "client/src/transport/gds_put_channel.h"
#include "client/src/transport/put_channel.h"
#include "client/src/transport/ucx_put_channel.h"

namespace us3_turbo::client {

namespace {

constexpr char kNotInitializedMsg[] =
    "Client is not initialized. Call Client::Initialize first.";

}  // namespace

Client::Client(ClientOptions options) : options_(std::move(options)) {}
Client::~Client() = default;

bool Client::Initialize() {
  if (initialized_) return true;

  // Mode B：单 channel 指向 proxy，承载 GdsPut / UcxPut。
  // brpc::Channel 线程安全、内部带连接复用，PutObject 重试与 bench 多 worker
  // 共享同一个 Client 时可并发调用。
  proxy_ = std::make_unique<ProxyRpc>(options_.endpoint, options_.default_timeout);
  if (!proxy_->ok()) {
    spdlog::error("Initialize: proxy channel({}) init failed: {}",
                  options_.endpoint, proxy_->init_error());
    proxy_.reset();
    return false;
  }

  // GDS 链路:取进程唯一 GdsMemoryManager,构 GdsPutChannel(channel 持有
  // manager 引用)。manager 不可用则该 channel 留空 + 告警,gds path 调用
  // 会在 SelectChannel 返回 nullptr 时失败。
  GdsMemoryManager* gds_mgr = nullptr;
  if (GdsMemoryManager::Instance(gds_mgr)) {
    gds_channel_ = std::make_unique<GdsPutChannel>(options_, *proxy_, gds_mgr);
  } else {
    spdlog::warn("Client::Initialize: GDS manager unavailable, "
                 "path=kGds will fail");
    gds_channel_.reset();
  }

  // UCX 链路:同构。Start 失败不致命:gds 链路仍可用,path=kUcx 的
  // PutObject 会在 SelectChannel 返回 nullptr 时失败。仅告警。
  UcxMemoryManager* ucx_mgr = nullptr;
  if (UcxMemoryManager::Instance(ucx_mgr)) {
    ucx_channel_ = std::make_unique<UcxPutChannel>(options_, *proxy_, ucx_mgr);
  } else {
    spdlog::warn("Client::Initialize: UCX manager unavailable, "
                 "path=kUcx will fail");
    ucx_channel_.reset();
  }

  initialized_ = true;
  return true;
}

void Client::Shutdown() {
  ucx_channel_.reset();
  gds_channel_.reset();
  proxy_.reset();
  initialized_ = false;
}

bool Client::initialized() const { return initialized_; }

// 公共重试模板：deadline 截止则放弃并记日志，否则交给 ExecuteWithRetry
// 按指数退避重试。模板只在 client.cpp 实例化（PutObject 分支），
// 定义置于唯一编译单元，不违反 ODR。
template <typename PutFunc>
bool Client::ExecutePutWithRetry(const ClientProxyPutRequest& request,
                                 std::string_view method_name,
                                 PutFunc&& put_operation) const {
  const auto deadline = std::chrono::steady_clock::now() + options_.request_timeout;

  return ExecuteWithRetry(RetryPolicy{}, [&]() -> bool {
    if (std::chrono::steady_clock::now() >= deadline) {
      spdlog::warn("{}: bucket={}/{} retry deadline exceeded",
                   method_name, request.bucket, request.key);
      return false;
    }
    return put_operation();
  });
}

// path 校验：kNone 拒绝（未指定通路），kAll 拒绝（推迟，单 buffer 无法双路）。
// source 不在此检查（由 channel 内部按 path 填充）。
bool Client::ValidatePutPath(const ClientProxyPutRequest& req) const {
  if (req.path == PutDataPath::kNone) {
    spdlog::error("PutObject: path not specified (req={})", req.request_id);
    return false;
  }
  if (HasPath(req.path, PutDataPath::kAll) &&
      req.path == PutDataPath::kAll) {
    spdlog::error("PutObject: kAll not supported yet (req={})", req.request_id);
    return false;
  }
  return true;
}

// 模式路由的唯一落点(见头注释)。
PutChannel* Client::SelectChannel(PutDataPath path) const noexcept {
  switch (path) {
    case PutDataPath::kGds: return gds_channel_.get();
    case PutDataPath::kUcx: return ucx_channel_.get();
    default:            return nullptr;  // kNone / kAll(已在校验阶段拒绝)
  }
}

bool Client::PutObject(const ClientProxyPutRequest& request,
                       ConstBufferView buffer,
                       ClientProxyPutResponse& response) const {
  if (!initialized_) {
    spdlog::error("PutObject: {} (req={})", kNotInitializedMsg, request.request_id);
    return false;
  }
  if (!ValidatePutPath(request)) {
    return false;
  }

  // 大小上限校验（沿用原 put_single_max_bytes）。
  const auto max_put = options_.put_single_max_bytes;
  if (max_put != 0 && buffer.size > max_put) {
    spdlog::warn("PutObject: bucket={}/{} body size {} exceeds put_single_max_bytes {}; "
                 "use multipart upload",
                 request.bucket, request.key, buffer.size, max_put);
    return false;
  }

  PutChannel* ch = SelectChannel(request.path);
  if (ch == nullptr) {
    // 与原"manager not initialized"日志语义对齐:链路不可用。
    spdlog::error("PutObject: {} channel not initialized (req={})",
                  HasPath(request.path, PutDataPath::kGds) ? "GDS" : "UCX",
                  request.request_id);
    return false;
  }

  return ExecutePutWithRetry(request, "PutObject", [&]() -> bool {
    PutPathResult r;
    const bool ok = ch->PutOnce(request, buffer, r);
    if (HasPath(request.path, PutDataPath::kGds)) response.gds_result = r;
    else                                        response.ucx_result = r;
    return ok;
  });
}

GdsPutChannel* Client::gds_channel() const noexcept {
  return gds_channel_.get();
}

}  // namespace us3_turbo::client
