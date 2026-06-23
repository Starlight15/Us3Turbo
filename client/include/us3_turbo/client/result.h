#pragma once

#include <optional>
#include <utility>

#include "us3_turbo/client/types.h"

namespace us3_turbo::client {

/**
 * @brief Error details returned by a failed client operation.
 */
struct Error {
  ErrorCode code{ErrorCode::kSuccess};
  std::string message;
  bool retryable{false};
  std::string failed_path;
  std::string request_id;
};

/**
 * @brief Typed result wrapper for public client operations.
 *
 * Callers must inspect `success()` before reading `value()` or `error()`.
 * When `success()` returns true, `value()` contains the operation result.
 * When `success()` returns false, `error()` contains failure details.
 */
template <typename T>
class Result {
 public:
  static Result Success(T value) { return Result(std::move(value)); }
  static Result Failure(Error error) { return Result(std::move(error)); }

  [[nodiscard]] bool success() const { return success_; }
  [[nodiscard]] const T& value() const { return *value_; }
  [[nodiscard]] T& value() { return *value_; }
  [[nodiscard]] const Error& error() const { return error_; }

 private:
  explicit Result(T value) : success_(true), value_(std::move(value)) {}
  explicit Result(Error error) : success_(false), error_(std::move(error)) {}

  bool success_{false};
  std::optional<T> value_{};
  Error error_{};
};

/**
 * @brief Constructs a normalized public error object.
 */
[[nodiscard]] inline Error MakeError(ErrorCode code, std::string message, bool retryable = false,
                                     std::string failed_path = {}, std::string request_id = {}) {
  return Error{.code = code,
               .message = std::move(message),
               .retryable = retryable,
               .failed_path = std::move(failed_path),
               .request_id = std::move(request_id)};
}

}  // namespace us3_turbo::client
