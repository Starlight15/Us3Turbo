#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace us3_turbo::client {

/**
 * @brief CRC-32C (Castagnoli) 软件实现,与 backend 端
 *        us3_turbo::gateway::common::Crc32c 算法完全一致
 *        (reflected 多项式 0x82F63B78 / init 0xFFFFFFFF / finalize ^0xFFFFFFFF)。
 *
 * 用于 GDS PUT 端到端一致性校验:client 对 device buffer 拷回 host 的内容算
 *        CRC32C,与 backend 在 GdsChunkResponse.crc32c 里回传的值比对。
 *
 * 两端各自维护一份实现,故意不共用,避免一端改动牵动另一端 wire 行为;
 *        任何对算法的修改都必须两端同步并通过 bench --verify-crc32c 校验。
 */
[[nodiscard]] std::uint32_t Crc32c(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::uint32_t Crc32c(std::string_view data) noexcept;

[[nodiscard]] std::uint32_t Crc32cInit() noexcept;
[[nodiscard]] std::uint32_t Crc32cUpdate(std::uint32_t state,
                                          const void* data, std::size_t n) noexcept;
[[nodiscard]] std::uint32_t Crc32cFinalize(std::uint32_t state) noexcept;

}  // namespace us3_turbo::client
