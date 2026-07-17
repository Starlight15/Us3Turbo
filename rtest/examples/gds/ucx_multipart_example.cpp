// ucx_multipart_example.cpp — UCX 分段上传端到端验证。
//
// host 内存走 UCX 链路：client → proxy(CreateMultipartUpload /
// UploadPartUcx / CompleteMultipartUpload) → backend(PutBlock，ucp_get_nbx
// 按 remote_addr+offset 反向拉取各 block)。
//
// 用法：
//   us3_turbo_ucx_multipart_example \
//     --proxy 192.168.1.198:9100 \
//     --part-size 4M --num-parts 3 [--verify-crc32c]

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "client/src/common/request.h"
#include "us3_turbo/client/client.h"

namespace {

using clk = std::chrono::steady_clock;
using ms_double = std::chrono::duration<double, std::milli>;

bool ParseSize(std::string_view s, std::uint64_t& out) {
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

std::string HumanBytes(std::uint64_t b) {
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

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = "192.168.1.198:9100";
  std::uint64_t part_size = 4ULL * 1024 * 1024;
  std::uint32_t num_parts = 3;
  bool verify = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need = [&](std::string& v) -> bool {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << arg << "\n";
        return false;
      }
      v = argv[++i];
      return true;
    };
    if (arg == "--proxy") {
      if (!need(proxy_addr)) return 2;
    } else if (arg == "--part-size") {
      std::string v;
      if (!need(v) || !ParseSize(v, part_size)) {
        std::cerr << "bad --part-size\n";
        return 2;
      }
    } else if (arg == "--num-parts") {
      std::string v;
      if (!need(v)) return 2;
      num_parts = static_cast<std::uint32_t>(std::strtoull(v.c_str(), nullptr, 10));
      if (num_parts == 0) {
        std::cerr << "bad --num-parts\n";
        return 2;
      }
    } else if (arg == "--verify-crc32c") {
      verify = true;
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  const std::uint64_t total = part_size * num_parts;
  std::cout << "=== UCX multipart ===\n"
            << "  proxy     : " << proxy_addr << "\n"
            << "  part size : " << HumanBytes(part_size) << "\n"
            << "  num parts : " << num_parts << "\n"
            << "  total     : " << HumanBytes(total) << "\n"
            << "  verify-crc: " << (verify ? "on" : "off") << "\n"
            << std::endl;

  std::vector<std::byte> host(part_size);
  for (std::size_t i = 0; i < part_size; ++i) host[i] = static_cast<std::byte>(i % 251U);

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.verify_crc32c = verify;

  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "Initialize failed\n";
    return 1;
  }

  std::string upload_id, error;
  if (!client.CreateMultipartUpload("test-bucket", "ucx-multipart.dat", PutDataPath::kUcx,
                                    upload_id, error)) {
    std::cerr << "CreateMultipartUpload failed: " << error << "\n";
    return 1;
  }
  std::cout << "CreateMultipartUpload: upload_id=" << upload_id << "\n";

  std::vector<Client::PartInfo> parts;
  parts.reserve(num_parts);

  const auto t0 = clk::now();
  for (std::uint32_t i = 1; i <= num_parts; ++i) {
    std::string etag;
    if (!client.UploadPartUcx(upload_id, i, ConstBufferView{.data = host.data(), .size = part_size},
                              etag, error)) {
      std::cerr << "UploadPartUcx " << i << " failed: " << error << "\n";
      return 1;
    }
    std::cout << "UploadPartUcx part " << i << " etag=" << etag << "\n";
    parts.push_back({i, etag});
  }

  Client::CompletedMultipart done;
  if (!client.CompleteMultipartUpload(upload_id, parts, done)) {
    std::cerr << "CompleteMultipartUpload failed: " << done.error << "\n";
    return 1;
  }
  const auto t1 = clk::now();
  const double wall_ms = ms_double(t1 - t0).count();

  std::cout << "CompleteMultipartUpload: object_id=" << done.object_id
            << " size=" << done.object_size << " etag=" << done.etag << "\n";
  const double wall_s = wall_ms / 1000.0;
  const double mbs = (wall_s > 0.0) ? static_cast<double>(total) / wall_s / (1024.0 * 1024.0) : 0.0;
  std::cout << "wall=" << wall_ms << "ms throughput=" << mbs << " MiB/s\n";

  client.Shutdown();

  if (done.object_size != total) {
    std::cerr << "size mismatch: got " << done.object_size << " want " << total << "\n";
    return 1;
  }
  return 0;
}
