#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace us3_turbo::gateway::common {

/**
 * CRC-32C (Castagnoli) 软件实现。多项式 0x1EDC6F41 reflected。
 * 用于 HTTP 数据通路 end-to-end 校验（S3 兼容的 x-amz-checksum-crc32c 头）。
 *
 * 用法：调 Crc32c(span) 拿整段；或 Crc32cInit() → 多次 Crc32cUpdate() → Crc32cFinalize()
 * 做流式（IOBuf 分块时用）。
 *
 * 实现独立：client 端 client/src/data/http_crc32c.{h,cpp} 各自维护一份。
 * 故意不共用，避免一端改动牵动另一端 wire 行为；任何对算法的修改都必须两端
 * 同步并通过 verify example 校验。
 *
 * 不引第三方：完全用 256 项查表 + 字节循环，简单可靠；V3 可换 _mm_crc32_u64
 * 硬件加速。
 */

[[nodiscard]] std::uint32_t Crc32c(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::uint32_t Crc32c(std::string_view data) noexcept;

[[nodiscard]] std::uint32_t Crc32cInit() noexcept;
[[nodiscard]] std::uint32_t Crc32cUpdate(std::uint32_t state,
                                          const void* data, std::size_t n) noexcept;
[[nodiscard]] std::uint32_t Crc32cFinalize(std::uint32_t state) noexcept;

/**
 * 将 base64 编码的 big-endian uint32 CRC32C 字符串解码为整数。
 * 与 S3 x-amz-checksum-crc32c header 格式兼容。
 * 解析失败返回 nullopt。
 */
[[nodiscard]] std::optional<std::uint32_t> ParseBase64Crc32c(std::string_view s) noexcept;

}  // namespace us3_turbo::gateway::common
