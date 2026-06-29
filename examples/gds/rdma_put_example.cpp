// rdma_put_example.cpp — RDMA(UCX) PUT 端到端验证。
//
// 与 gds_put_example 对应：用 host 内存（非 device 显存）走 RDMA(UCX) 链路。
// client → proxy → backend（backend ucp_get_nbx 反向拉 client host 内存）。
//
// 用法：
//   us3_turbo_rdma_put_example --proxy 192.168.1.198:9100 [--size 100M] [--verify-crc32c]

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "us3_turbo/client/client.h"

namespace {

bool ParseSize(const std::string& s, std::uint64_t& out) {
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
      case 'b': mul = 1ULL; break;
      case 'k': mul = 1024ULL; break;
      case 'm': mul = 1024ULL * 1024; break;
      case 'g': mul = 1024ULL * 1024 * 1024; break;
      default: return false;
    }
  }
  out = num * mul;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = "192.168.1.198:9100";
  std::uint64_t bytes = 100ULL * 1024ULL * 1024ULL;
  bool verify = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need = [&](std::string& v) -> bool {
      if (i + 1 >= argc) { std::cerr << "missing value for " << arg << "\n"; return false; }
      v = argv[++i]; return true;
    };
    if (arg == "--proxy") { if (!need(proxy_addr)) return 2; }
    else if (arg == "--size") {
      std::string v; if (!need(v) || !ParseSize(v, bytes)) { std::cerr << "bad --size\n"; return 2; }
    }
    else if (arg == "--verify-crc32c") { verify = true; }
    else { std::cerr << "unknown arg: " << arg << "\n"; return 2; }
  }

  std::vector<std::byte> host(bytes);
  for (std::size_t i = 0; i < bytes; ++i) host[i] = static_cast<std::byte>(i % 251U);

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.verify_crc32c = verify;

  Client client(std::move(opts));
  if (!client.Initialize()) { std::cerr << "Initialize failed\n"; return 1; }

  PutObjectRequest req;
  req.set_bucket("test-bucket").set_key("obj-rdma-1").set_expected_size(bytes);

  TransferOutcome outcome;
  bool ok = client.PutObjectRdma(req, ConstBufferView{.data = host.data(), .size = bytes}, outcome);

  client.Shutdown();

  if (!ok) { std::cerr << "PutObjectRdma FAILED\n"; return 1; }
  std::cout << "OK bytes=" << outcome.bytes_transferred
            << " etag=" << outcome.etag
            << " crc32c=" << std::hex << outcome.crc32c << std::dec << "\n";
  return 0;
}
