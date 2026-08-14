// rdma_put_example.cpp — RDMA 单步 PUT 最简示例。

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy = rtest::kDefaultProxyEndpoint;
  std::string rdma_bind_ip;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--proxy" && i + 1 < argc) {
      proxy = argv[++i];
    } else if (a == "--rdma-bind-ip" && i + 1 < argc) {
      rdma_bind_ip = argv[++i];
    } else {
      std::cerr << "usage: " << argv[0] << " [--proxy HOST:PORT] [--rdma-bind-ip IP]\n";
      return 2;
    }
  }
  constexpr const char* kBucket = "test-bucket";
  constexpr std::uint64_t kSize = 4ULL * 1024 * 1024;
  // 时间戳随机后缀，避免多次运行 key 冲突。
  const std::string key = "rdma-demo-" + rtest::MakeTimestampSuffix();

  std::vector<std::byte> host(kSize);
  rtest::FillHostPattern(host);

  Client client(ClientOptions{.endpoint = proxy, .rdma_bind_ip = rdma_bind_ip});
  client.Initialize();

  ClientProxyPutRequest put_req;
  put_req.bucket = kBucket;
  put_req.key = key;
  put_req.object_size = kSize;
  put_req.path = PutDataPath::kRdma;

  ClientProxyPutResponse resp;
  client.PutObjectRdma(put_req, ConstBufferView{.data = host.data(), .size = kSize}, resp);

  client.Shutdown();

  const auto& r = resp.rdma_result.value();
  if (r.bytes == kSize) {
    std::cout << "OK etag=" << r.etag << " crc32c=0x" << std::hex << r.crc32c << std::dec << "\n";
    return 0;
  }
  std::cerr << "FAILED\n";
  return 1;
}
