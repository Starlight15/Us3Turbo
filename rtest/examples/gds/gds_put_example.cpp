// gds_put_example.cpp — GDS 单步 PUT 最简示例。

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  const char* kProxy = (argc > 1) ? argv[1] : rtest::kDefaultProxyEndpoint;
  constexpr const char* kBucket = "test-bucket";
  constexpr std::uint64_t kSize = 4ULL * 1024 * 1024;

  void* dev = nullptr;
  cudaMalloc(&dev, kSize);
  std::vector<std::byte> host(kSize);
  rtest::FillHostPattern(host);
  cudaMemcpy(dev, host.data(), kSize, cudaMemcpyHostToDevice);

  Client client(ClientOptions{.endpoint = kProxy});
  client.Initialize();

  ClientProxyPutRequest put_req;
  put_req.bucket = kBucket;
  put_req.key = "gds-demo";
  put_req.object_size = kSize;
  put_req.path = PutDataPath::kGds;

  ClientProxyPutResponse resp;
  client.PutObjectGds(put_req, ConstBufferView{.data = dev, .size = kSize}, resp);

  cudaFree(dev);
  client.Shutdown();

  const auto& r = resp.gds_result.value();
  if (r.bytes == kSize) {
    std::cout << "OK etag=" << r.etag << " crc32c=0x" << std::hex << r.crc32c << std::dec << "\n";
    return 0;
  }
  std::cerr << "FAILED\n";
  return 1;
}
