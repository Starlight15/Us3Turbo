#pragma once

#include <string>
#include <utility>

#include "us3_turbo/client/result.h"  // for Error / ErrorCode / MakeError

namespace us3_turbo::client {

// Status —— 轻量"成功/失败"对象，配合 out-param 使用。
//
// 与 Result<T> 共存：
//   - Result<T> 用于"成功时携带返回值"的接口。
//   - Status   用于"成功时通过 out-param 写回，失败时只回报错误"的接口，
//              避免调用方写 auto x = api(...); auto& v = x.value(); 这种二段式拆包。
//
// Status 复用现有 Error 类型（code / message / retryable / failed_path / request_id），
// 因此调用方拿到的失败信息与 Result<T> 完全一致，可以与现有错误处理代码自由互转。
class Status {
 public:
  // 默认构造 = 成功。
  Status() = default;

  // 从一个 Error 构造失败状态。code 为 kSuccess 时仍视为成功。
  explicit Status(Error error) : ok_(error.code == ErrorCode::kSuccess), error_(std::move(error)) {}

  static Status Ok() { return Status{}; }
  static Status FromError(Error error) { return Status(std::move(error)); }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

 private:
  bool  ok_{true};
  Error error_{};
};

}  // namespace us3_turbo::client
