#include "client/src/data/crc32c.h"

#include <array>

namespace us3_turbo::client {

namespace {

// Castagnoli reflected polynomial 0x82F63B78(== bit-reverse(0x1EDC6F41))。
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

}  // namespace us3_turbo::client
