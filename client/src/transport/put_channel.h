#pragma once

// put_channel.h — 链路抽象(路由落点)。

#include "client/src/common/request.h"
#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/** @brief 单条 PUT 链路抽象:PutOnce 为一次尝试(不含重试),重试由 Client 负责。 */
class PutChannel {
 public:
  virtual ~PutChannel() = default;

  /** @brief 单次 PUT 尝试:result 回填链路结果,失败返回 false 供上层重试。 */
  [[nodiscard]] virtual bool PutOnce(const ClientProxyPutRequest& request,
                                     ConstBufferView buffer,
                                     PutPathResult& result) const = 0;
};

}  // namespace us3_turbo::client
