#pragma once

#include <string_view>

namespace us3_turbo::common {

/**
 * @brief Canonical error codes shared between the client SDK and the gateway.
 *
 * The values 0..kTransportError are stable and consumed by the client.
 * Codes >= kNotFound are server-side additions that gateway components emit
 * when the operation cannot be classified into a client-facing category yet.
 */
enum class ErrorCode {
  // Shared with client (kept stable in declaration order for ABI continuity).
  kSuccess = 0,
  kInvalidArgument,
  kUnsupported,
  kInternal,
  kRpcError,
  kSerializationError,
  kControlPlaneError,
  kRegistrationFailed,
  kTransportError,

  // Server-side additions.
  kNotFound,
  kBadRequest,
  kRangeNotSatisfiable,
  kBackendUnavailable,
  kCapacityExceeded,
  kRdmaUnavailable,
  kSessionNotFound,
  kTicketInvalid,
  kStaleState,
  kPayloadTooLarge,  // HTTP 413: PUT body exceeds gateway/client size limit
  kMethodNotAllowed, // HTTP 405: Method not allowed
  kTimeout,          // 请求超时（端到端 deadline 触发；retryable=true）
};

[[nodiscard]] constexpr std::string_view ToString(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kSuccess:             return "success";
    case ErrorCode::kInvalidArgument:     return "invalid_argument";
    case ErrorCode::kUnsupported:         return "unsupported";
    case ErrorCode::kInternal:            return "internal";
    case ErrorCode::kRpcError:           return "rpc_error";
    case ErrorCode::kSerializationError:  return "serialization_error";
    case ErrorCode::kControlPlaneError:   return "control_plane_error";
    case ErrorCode::kRegistrationFailed:  return "registration_failed";
    case ErrorCode::kTransportError:      return "transport_error";
    case ErrorCode::kNotFound:            return "not_found";
    case ErrorCode::kBadRequest:          return "bad_request";
    case ErrorCode::kRangeNotSatisfiable: return "range_not_satisfiable";
    case ErrorCode::kBackendUnavailable:  return "backend_unavailable";
    case ErrorCode::kCapacityExceeded:    return "capacity_exceeded";
    case ErrorCode::kRdmaUnavailable:     return "rdma_unavailable";
    case ErrorCode::kSessionNotFound:     return "session_not_found";
    case ErrorCode::kTicketInvalid:       return "ticket_invalid";
    case ErrorCode::kStaleState:          return "stale_state";
    case ErrorCode::kPayloadTooLarge:     return "payload_too_large";
    case ErrorCode::kMethodNotAllowed:    return "method_not_allowed";
    case ErrorCode::kTimeout:             return "timeout";
  }
  return "unknown";
}

}  // namespace us3_turbo::common
