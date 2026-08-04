#pragma once

#include <cstdint>
#include <span>

namespace us3_turbo::client {

/**
 * @brief CRC-32C(Castagnoli)软件实现,与 backend 端 Crc32c 一致。
 * 用于 PUT 端到端校验:client 算 CRC32C 与 backend 回传的 PutPathResult.crc32c
 * 比对。
 */
[[nodiscard]] std::uint32_t Crc32c(std::span<const std::byte> data) noexcept;

}  // namespace us3_turbo::client
