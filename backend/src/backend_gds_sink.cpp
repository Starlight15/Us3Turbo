#include "backend/src/backend_gds_sink.h"

#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <infiniband/verbs.h>
#include <spdlog/spdlog.h>

#include "common/crc32c.h"
#include "gds/cuobj_resources.h"

namespace us3_turbo::backend {

namespace {

// 与 gateway GdsOptions 默认值对齐：1MB / 16MB / 256MB / 1GB，每 class 4 份。
// 读自 gateway/include/us3_turbo/gateway/options.h，v1 照填。
constexpr std::size_t kMaxChunkBytes = 1ULL * 1024ULL * 1024ULL * 1024ULL;  // 1 GiB

const std::vector<std::size_t>& BufferSizeClasses() {
  static const std::vector<std::size_t> classes{
      1ULL * 1024ULL * 1024ULL,
      16ULL * 1024ULL * 1024ULL,
      256ULL * 1024ULL * 1024ULL,
      1ULL * 1024ULL * 1024ULL * 1024ULL,
  };
  return classes;
}
constexpr std::size_t kBufferMaxPerClass = 4;

// 照抄 gds_executor.cpp:32-40：token 形如 "hexaddr:rkey"，取冒号前的 16 进制地址。
[[nodiscard]] std::uint64_t ParseRemoteBufferAddress(const std::string& token) {
  const auto colon = token.find(':');
  const std::string hex =
      colon == std::string::npos ? token : token.substr(0, colon);
  if (hex.empty()) {
    return 0;
  }
  return std::strtoull(hex.c_str(), nullptr, 16);
}

[[nodiscard]] std::string DescribeStatus(ibv_wc_status status) {
  const char* description = ibv_wc_status_str(status);
  if (description == nullptr) {
    return std::to_string(static_cast<int>(status));
  }
  return description;
}

}  // namespace

BackendGdsSink::BackendGdsSink(std::string bind_host, int rdma_port)
    : bind_host_(std::move(bind_host)), rdma_port_(rdma_port) {}

BackendGdsSink::~BackendGdsSink() { Stop(); }

bool BackendGdsSink::Start() {
  auto server = std::make_shared<cuObjServer>(
      bind_host_.c_str(), static_cast<unsigned short>(rdma_port_),
      CUOBJ_PROTO_RDMA_DC_V1);
  if (!server->isConnected()) {
    spdlog::error("backend: cuObjServer init failed on {}:{}", bind_host_,
                  rdma_port_);
    return false;
  }
  server_ = std::move(server);
  // pool 析构需 server 存活，见 Stop()。
  pool_ = std::make_shared<
      us3_turbo::gateway::data_flow::gds::PinnedBufferPool>(
      *server_, BufferSizeClasses(), kBufferMaxPerClass);
  spdlog::info("backend: cuObjServer on {}:{}", bind_host_, rdma_port_);
  return true;
}

void BackendGdsSink::Stop() {
  // 先销毁 pool（要在 server 还活着时调 deRegisterBuffer），再 reset server。
  std::shared_ptr<us3_turbo::gateway::data_flow::gds::PinnedBufferPool>
      pool_to_destroy;
  pool_to_destroy = std::move(pool_);
  if (pool_to_destroy) {
    pool_to_destroy->Shutdown();
  }
  pool_to_destroy.reset();
  server_.reset();
}

bool BackendGdsSink::available() const {
  return server_ != nullptr && server_->isConnected();
}

DiscardOutcome BackendGdsSink::ReceiveAndDiscard(const std::string& object_id,
                                                 const std::string& rdma_token,
                                                 std::uint64_t length) {
  using namespace us3_turbo::gateway::data_flow::gds;
  DiscardOutcome outcome;

  if (length > kMaxChunkBytes) {
    outcome.error = "GDS PUT chunk exceeds 1 GiB cuObjServer limit";
    return outcome;
  }
  if (length == 0U) {
    outcome.ok = true;
    outcome.bytes_transferred = 0;
    outcome.crc32c = 0;
    return outcome;
  }

  PinnedBufferLease lease;
  if (pool_) {
    lease = pool_->Acquire(static_cast<std::size_t>(length));
  }
  if (!lease.ok()) {
    outcome.error = "cuObjServer pinned buffer alloc failed";
    return outcome;
  }

  const auto channel = server_->allocateChannelId();
  if (channel == INVALID_CHANNEL_ID) {
    outcome.error = "cuObjServer allocateChannelId failed";
    return outcome;
  }
  ChannelGuard chan_guard(*server_, channel);

  const auto remote_buf_start = ParseRemoteBufferAddress(rdma_token);
  ibv_wc_status status = IBV_WC_SUCCESS;
  const auto transferred = server_->handlePutObject(
      object_id, lease.mr(), remote_buf_start,
      static_cast<std::size_t>(length), rdma_token, channel, 0, &status,
      nullptr);
  spdlog::info(
      "backend.put object={} length={} transferred={} status={}",
      object_id, length, transferred, DescribeStatus(status));

  if (transferred < 0) {
    outcome.error =
        "cuObjServer handlePutObject failed: " + DescribeStatus(status);
    return outcome;
  }

  const auto bytes = static_cast<std::size_t>(transferred);
  // 对前 transferred 字节算 CRC32C（复用 gateway common::Crc32c），供端到端校验。
  outcome.crc32c = us3_turbo::gateway::common::Crc32c(
      std::span<const std::byte>(static_cast<const std::byte*>(lease.data()),
                                 bytes));
  // 【丢弃】不接 IDataStore、不写盘。
  outcome.ok = true;
  outcome.bytes_transferred = static_cast<std::uint64_t>(bytes);
  return outcome;
}

}  // namespace us3_turbo::backend
