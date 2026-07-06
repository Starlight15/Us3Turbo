#pragma once

#include <string>
#include <utility>

#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

// 服务层统一返回类型：code==0 成功，非 0 为 PROXY_ERR_*。
// 接口层据此填 response + cntl->SetFailed；不把 brpc 概念泄漏进服务层。
struct ProxyStatus {
  int         code{0};
  std::string message;

  [[nodiscard]] bool ok() const { return code == 0; }

  static ProxyStatus Ok() { return {}; }

  static ProxyStatus Fail(int code, std::string msg) {
    return {code, std::move(msg)};
  }
};

}  // namespace us3_turbo::proxy
