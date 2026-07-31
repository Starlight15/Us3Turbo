#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace us3_turbo::client {

/**
 * @brief CRC-32C(Castagnoli)软件实现,与 backend 端 Crc32c 一致。
 * 用于 PUT 端到端校验:client 算 CRC32C 与 backend 回传的 PutPathResult.crc32c
 * 比对。
 */
[[nodiscard]] std::uint32_t Crc32c(std::span<const std::byte> data) noexcept;

[[nodiscard]] std::uint32_t Crc32cInit() noexcept;

[[nodiscard]] std::uint32_t Crc32cUpdate(std::uint32_t state, const void* data,
                                         std::size_t n) noexcept;

[[nodiscard]] std::uint32_t Crc32cFinalize(std::uint32_t state) noexcept;

}  // namespace us3_turbo::client
