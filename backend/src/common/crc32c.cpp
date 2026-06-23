#include "common/crc32c.h"

#include <array>
#include <vector>

namespace us3_turbo::gateway::common {

namespace {

// Castagnoli reflected polynomial 0x82F63B78（== bit-reverse(0x1EDC6F41)）。
constexpr std::uint32_t kPoly = 0x82F63B78u;

constexpr std::array<std::uint32_t, 256> MakeTable() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1U) ? ((c >> 1) ^ kPoly) : (c >> 1);
    }
    t[i] = c;
  }
  return t;
}

constexpr auto kTable = MakeTable();

}  // namespace

std::uint32_t Crc32cInit() noexcept { return 0xFFFFFFFFu; }

std::uint32_t Crc32cUpdate(std::uint32_t state, const void* data,
                            std::size_t n) noexcept {
  const auto* p = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < n; ++i) {
    state = (state >> 8) ^ kTable[(state ^ p[i]) & 0xFFu];
  }
  return state;
}

std::uint32_t Crc32cFinalize(std::uint32_t state) noexcept {
  return state ^ 0xFFFFFFFFu;
}

std::uint32_t Crc32c(std::span<const std::byte> data) noexcept {
  return Crc32cFinalize(
      Crc32cUpdate(Crc32cInit(), data.data(), data.size()));
}

std::uint32_t Crc32c(std::string_view data) noexcept {
  return Crc32cFinalize(
      Crc32cUpdate(Crc32cInit(), data.data(), data.size()));
}

std::optional<std::uint32_t> ParseBase64Crc32c(std::string_view s) noexcept {
  if (s.empty()) return std::nullopt;
  auto idx = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62; if (c == '/') return 63; return -1;
  };
  std::vector<std::uint8_t> out;
  std::uint32_t buf = 0; int bits = 0;
  for (char c : s) {
    if (c == '=') break;
    int v = idx(c); if (v < 0) return std::nullopt;
    buf = (buf << 6) | static_cast<std::uint32_t>(v); bits += 6;
    if (bits >= 8) { bits -= 8; out.push_back(static_cast<std::uint8_t>((buf >> bits) & 0xFFu)); }
  }
  if (out.size() < 4) return std::nullopt;
  return (std::uint32_t(out[0]) << 24) | (std::uint32_t(out[1]) << 16) |
         (std::uint32_t(out[2]) << 8)  |  std::uint32_t(out[3]);
}

}  // namespace us3_turbo::gateway::common
