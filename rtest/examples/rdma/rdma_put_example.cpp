// rdma_put_example.cpp — RDMA 单步 PUT 最简示例。
//
// 演示: host buffer → PutObjectRdma → 打印结果。
// 运行: us3_turbo_rdma_put_example [proxy_addr]

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

// ---- 本地常量 ----
constexpr const char* kTestProxy = "192.168.1.198:9100";
constexpr const char* kTestBucket = "test-bucket";
constexpr std::uint64_t kTestObjectSize = 4ULL * 1024 * 1024;

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  const std::string proxy_addr = (argc > 1) ? argv[1] : kTestProxy;
  constexpr std::uint64_t kSize = kTestObjectSize;

  // 1. 准备 host buffer
  std::vector<std::byte> host(kSize);
  rtest::FillHostPattern(host);

  // 2. 初始化 client
  Client client(ClientOptions{.endpoint = proxy_addr});
  client.Initialize();

  // 3. PUT
  ClientProxyPutResponse resp;
  client.PutObjectRdma(ClientProxyPutRequest{.bucket = kTestBucket,
                                              .key = "rdma-demo",
                                              .object_size = kSize,
                                              .path = PutDataPath::kRdma},
                        ConstBufferView{.data = host.data(), .size = kSize}, resp);

  client.Shutdown();

  const auto& r = resp.rdma_result.value();
  if (r.bytes_written == kSize) {
    std::cout << "OK etag=" << r.etag << " crc32c=0x" << std::hex << r.crc32c << std::dec << "\n";
    return 0;
  }
  std::cerr << "FAILED\n";
  return 1;
}
