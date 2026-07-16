#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "client/src/common/request.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

namespace {

// 解析 "16M"/"17M"/"100M" 等，支持 B/K/M/G（1024 进制）。
bool ParseSize(std::string_view s, std::size_t& out) {
  if (s.empty()) return false;
  std::size_t num = 0;
  std::size_t i = 0;
  for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++i) {
    num = num * 10 + static_cast<std::size_t>(s[i] - '0');
  }
  if (i == 0) return false;
  std::size_t mul = 1;
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

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  const std::string proxy_addr = "192.168.1.198:9100";
  std::size_t bytes =
      100UL * 1024UL * 1024UL;  // 默认 100M（超 16M 单步上限，用于演示拒绝）
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--size") {
      if (i + 1 >= argc) {
        std::cerr << "missing value for --size\n";
        return 2;
      }
      if (!ParseSize(argv[++i], bytes)) {
        std::cerr << "bad --size\n";
        return 2;
      }
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  void* dev = nullptr;
  cudaError_t e = cudaMalloc(&dev, bytes);
  if (e != cudaSuccess) {
    std::cerr << "cudaMalloc: " << cudaGetErrorString(e) << "\n";
    return 1;
  }
  std::vector<std::byte> host(bytes);
  for (std::size_t i = 0; i < bytes; ++i)
    host[i] = static_cast<std::byte>(i % 251U);
  e = cudaMemcpy(dev, host.data(), bytes, cudaMemcpyHostToDevice);
  if (e != cudaSuccess) {
    std::cerr << "cudaMemcpy: " << cudaGetErrorString(e) << "\n";
    cudaFree(dev);
    return 1;
  }

  ClientOptions opts;
  opts.endpoint = proxy_addr;

  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "Initialize failed\n";
    cudaFree(dev);
    return 1;
  }

  ClientProxyPutRequest req;
  req.bucket = "test-bucket";
  req.key = "obj1";
  req.object_size = bytes;
  req.path = PutDataPath::kGds;

  ClientProxyPutResponse resp;
  bool put_ok =
      client.PutObject(req, ConstBufferView{.data = dev, .size = bytes}, resp);

  cudaFree(dev);

  if (!put_ok) {
    std::cerr << "PutObject FAILED\n";
    return 1;
  }
  const auto& r = resp.gds_result.value();
  std::cout << "OK bytes=" << r.bytes_written << " etag=" << r.etag << "\n";
  return 0;
}
