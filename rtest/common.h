// rtest/common.h — 回归测试与示例的共享 helper（通路无关，header-only，无 CUDA
// 依赖）。GDS/RDMA 各通路通用。
//
// 提供 ParseSize / HumanBytes / FillHostPattern / VerifyHostBuffer /
// MakeTimestampSuffix 等工具，避免在 examples/bench/regression 中重复实现。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>


namespace rtest {
// 分段上传默认 part 大小，须与 proxy FLAGS_multipart_part_size /
// ClientOptions::multipart_part_size 一致。
constexpr std::uint64_t kDefaultPartSize = 16ULL * 1024 * 1024;  // 16 MiB

// 默认 proxy 控制面 endpoint。
constexpr const char* kDefaultProxyEndpoint = "192.168.1.198:9100";



// 解析 "4M"/"1M" 等（1024 进制）。签名对齐 multipart/get examples。
inline bool ParseSize(std::string_view s, std::uint64_t& out) {
  if (s.empty()) return false;
  std::uint64_t num = 0;
  std::size_t i = 0;
  for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++i) {
    num = num * 10 + static_cast<std::uint64_t>(s[i] - '0');
  }
  if (i == 0) return false;
  std::uint64_t mul = 1;
  if (i < s.size()) {
    if (i + 1 != s.size()) return false;
    switch (std::tolower(static_cast<unsigned char>(s[i]))) {
      case 'b':
        mul = 1ULL;
        break;
      case 'k':
        mul = 1024ULL;
        break;
      case 'm':
        mul = 1024ULL * 1024;
        break;
      case 'g':
        mul = 1024ULL * 1024 * 1024;
        break;
      default:
        return false;
    }
  }
  out = num * mul;
  return true;
}

inline std::string HumanBytes(std::uint64_t b) {
  constexpr double K = 1024.0;
  char buf[64];
  if (b >= static_cast<std::uint64_t>(K * K * K))
    std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(b) / (K * K * K));
  else if (b >= static_cast<std::uint64_t>(K * K))
    std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(b) / (K * K));
  else if (b >= static_cast<std::uint64_t>(K))
    std::snprintf(buf, sizeof(buf), "%.2f KiB", static_cast<double>(b) / K);
  else
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(b));
  return buf;
}

// 时间戳后缀，避免多次运行 key 冲突（同 gds_get_example 做法）。
inline std::string MakeTimestampSuffix() {
  return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}

// 确定性 i%251 pattern 填入 host buffer（GDS 作 H2D 暂存
// buffer）。offset_base 让多 part 对象各段填不同 pattern。
inline void FillHostPattern(std::vector<std::byte>& buf, std::uint64_t offset_base = 0) {
  for (std::size_t i = 0; i < buf.size(); ++i)
    buf[i] = static_cast<std::byte>((i + offset_base) % 251U);
}

// 逐字节比对 host 读回 buffer 与期望；打印首处失配细节。GDS 调用前需 D2H。
inline bool VerifyHostBuffer(const void* read, std::size_t size,
                             const std::vector<std::byte>& expected, const std::string& tag) {
  const auto* p = static_cast<const std::byte*>(read);
  if (size != expected.size()) {
    std::cerr << "[" << tag << "] size mismatch: got " << size << " want " << expected.size()
              << "\n";
    return false;
  }
  std::size_t mism = 0;
  std::size_t first = 0;
  for (std::size_t i = 0; i < size; ++i) {
    if (p[i] != expected[i]) {
      if (mism == 0) first = i;
      ++mism;
    }
  }
  if (mism > 0) {
    std::cerr << "[" << tag << "] DATA MISMATCH: " << mism << " bytes differ, first at offset "
              << first << " (got 0x" << std::hex << static_cast<unsigned>(p[first]) << " want 0x"
              << static_cast<unsigned>(expected[first]) << std::dec << ")\n";
    return false;
  }
  std::cout << "[" << tag << "] data VERIFIED OK (" << HumanBytes(size) << ")\n";
  return true;
}

}  // namespace rtest
