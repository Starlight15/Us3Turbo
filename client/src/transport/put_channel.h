#pragma once

// put_channel.h — 链路抽象(路由落点)。干净共享头,不含 cuObj/ucp 痕迹。

#include "client/src/contracts/put_request.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief 单条 PUT 链路的抽象接口。
 *
 * PutOnce 为一次 PUT 尝试(不含重试):描述符获取 → proxy RPC → 可选 CRC/trace,
 * 结果回填 result。重试由上层 Client::PutObject 负责,channel 只做单发。
 */
class PutChannel {
 public:
  virtual ~PutChannel() = default;

  /**
   * @brief 单次 PUT 尝试。
   * @param request 请求(bucket/key/size/path;request_id 由实现内部每次新生成)。
   * @param buffer  数据 buffer(GDS=device 显存,UCX=host)。
   * @param result  [out] 链路结果(ok/error/etag/crc32c/bytes_written)。
   * @return true 成功;false 失败(上层据此重试)。
   */
  [[nodiscard]] virtual bool PutOnce(const ClientProxyPutRequest& request,
                                     ConstBufferView buffer,
                                     PutPathResult& result) const = 0;
};

}  // namespace us3_turbo::client
