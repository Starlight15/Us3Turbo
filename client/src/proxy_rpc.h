#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "client/src/common/rpc_base.h"
#include "client/src/contracts/put_request.h"
#include "us3_turbo/client/options.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief proxy 控制面 RPC client（Mode B）。
 *
 * client 只与 proxy 交互：GdsPut / UcxPut 走同一条 brpc channel（指向
 * options.endpoint=proxy），单次 RPC 自带描述符。protobuf 响应类型只在
 * proxy_rpc.cpp 内部出现，对外只回填客户端自有的 PutPathResult。
 *
 * 调用侧每次（含每次重试）生成新 request_id 传入；bucket/key/size 直接
 * 取自 ClientProxyPutRequest。两个方法独立，便于 GDS 机器稀缺时单独跳过。
 *
 * 单 channel 足够：brpc::Channel 线程安全、内部带连接复用，可被多线程
 * 并发调用（PutObject 重试 / bench 多 worker 共享同一个 Client）。
 */
class ProxyRpc : public RpcBase {
 public:
  ProxyRpc(const std::string& endpoint, std::chrono::milliseconds timeout)
      : RpcBase(endpoint, timeout, "proxy") {}

  /**
   * @brief GDS 通路的 GdsPut：把 client 的 cuObj RDMA token 随 RPC 透传给
   *        proxy，proxy 转发 backend，backend 反向 RDMA-READ。与 UcxPut 独立。
   * @return true RPC 层成功（result.ok 反映 backend 执行结果）。
   */
  [[nodiscard]] bool GdsPut(std::string_view request_id,
                            const std::string& bucket,
                            const std::string& key,
                            std::uint64_t object_size,
                            const GdsDataSource& gds_source,
                            PutPathResult& result) const;

  /**
   * @brief UCX 通路的 UcxPut：把 client 的 UCX 描述符（remote_addr /
   *        packed rkey / client_ucx_addr）随 RPC 透传给 proxy，proxy 转发
   *        backend，backend ucp_get_nbx 反向拉取。与 GdsPut 完全独立。
   */
  [[nodiscard]] bool UcxPut(std::string_view request_id,
                            const std::string& bucket,
                            const std::string& key,
                            std::uint64_t object_size,
                            const UcxDataSource& ucx_source,
                            PutPathResult& result) const;
};

}  // namespace us3_turbo::client
