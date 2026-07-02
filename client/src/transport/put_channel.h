#pragma once

// put_channel.h — Client 唯一依赖的链路抽象（模式路由的落点）。
//
// Mode B 的目标:调用方只设 req.path = kGds / kUcx(未来 kAll),Client
// 按 path 选一条 PutChannel,链路细节(GDS device token / UCX host 描述符)
// 藏在各自的 channel 实现里。本头是「干净」共享头,不含 cuObj / ucp 任何
// 痕迹——GDS 单独回归不会被 UCX 依赖污染,反之亦然(见 review/
// client_refactor_prompt.md 硬约束)。

#include "client/src/contracts/put_request.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief 单条 PUT 链路的抽象接口。
 *
 * PutOnce 一次 PUT 尝试(不含重试):内部完成 描述符获取 → proxy RPC →
 * 可选 CRC → 可选 trace,把链路执行结果回填到 @p result。
 *
 * 调用方(Client::PutObject)负责 deadline 截断与重试,channel 只做单发。
 * result.ok 反映链路执行结果;false 触发上层重试。
 */
class PutChannel {
 public:
  virtual ~PutChannel() = default;

  /**
   * @brief 单次 PUT 尝试。
   * @param request  请求(bucket/key/size/path,request_id 由实现内部按
   *                 「每次尝试新 request_id」语义生成,跨端日志关联用)。
   * @param buffer   数据 buffer(GDS 链路要求 device 显存,UCX 链路要求 host)。
   * @param result   [out] 链路执行结果(ok/error/etag/crc32c/bytes_written)。
   * @return true 链路执行成功(result.ok=true);false 失败(上层据此重试)。
   */
  [[nodiscard]] virtual bool PutOnce(const ClientProxyPutRequest& request,
                                     ConstBufferView buffer,
                                     PutPathResult& result) const = 0;
};

}  // namespace us3_turbo::client
