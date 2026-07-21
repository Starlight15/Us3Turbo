// gds_put_example.cpp — GDS 单步 PUT 最简示例。
//
// 演示: cudaMalloc → H2D → PutObjectGds → 打印结果。
// 运行: us3_turbo_gds_put_example [proxy_addr]

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

  const std::string proxy_addr = (argc > 1) ? argv[1] : rtest::kDefaultProxy;
  constexpr std::uint64_t kSize = rtest::kDefaultObjectSize;

  // 1. 分配 GPU buffer + 填充测试数据
  void* dev = nullptr;
  cudaMalloc(&dev, kSize);
  std::vector<std::byte> host(kSize);
  rtest::FillHostPattern(host);
  cudaMemcpy(dev, host.data(), kSize, cudaMemcpyHostToDevice);

  // 2. 初始化 client
  Client client(ClientOptions{.endpoint = proxy_addr});
  client.Initialize();

  // 3. PUT
  ClientProxyPutResponse resp;
  client.PutObjectGds(ClientProxyPutRequest{.bucket = rtest::kDefaultBucket,
                                             .key = "gds-demo",
                                             .object_size = kSize,
                                             .path = PutDataPath::kGds},
                       ConstBufferView{.data = dev, .size = kSize}, resp);

  cudaFree(dev);
  client.Shutdown();

  const auto& r = resp.gds_result.value();
  if (r.bytes_written == kSize) {
    std::cout << "OK etag=" << r.etag << " crc32c=0x" << std::hex << r.crc32c << std::dec << "\n";
    return 0;
  }
  std::cerr << "FAILED\n";
  return 1;
}
