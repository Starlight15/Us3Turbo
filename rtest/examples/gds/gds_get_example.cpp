// gds_get_example.cpp — GDS PUT + GET 读回验证最简示例。
//
// 演示: PutObjectGds → StatObject → GetObjectGds → D2H 逐字节比对。
// 运行: us3_turbo_gds_get_example [proxy_addr]

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  const std::string proxy_addr = (argc > 1) ? argv[1] : "192.168.1.198:9100";
  constexpr std::uint64_t kSize = 4ULL * 1024 * 1024;  // 4 MiB

  // 1. 分配 PUT buffer + GET buffer（同时存活，避免 cuObj descriptor 失效）
  void* dev_put = nullptr;
  void* dev_get = nullptr;
  cudaMalloc(&dev_put, kSize);
  cudaMalloc(&dev_get, kSize);

  // 2. 填充 PUT buffer
  std::vector<std::byte> host(kSize);
  rtest::FillHostPattern(host);
  cudaMemcpy(dev_put, host.data(), kSize, cudaMemcpyHostToDevice);

  // 3. 初始化 client
  Client client(ClientOptions{.endpoint = proxy_addr});
  client.Initialize();

  // 4. PUT
  ClientProxyPutResponse put_resp;
  client.PutObjectGds(ClientProxyPutRequest{.bucket = "test-bucket",
                                             .key = "gds-get-demo",
                                             .object_size = kSize,
                                             .path = PutDataPath::kGds},
                       ConstBufferView{.data = dev_put, .size = kSize}, put_resp);
  std::cout << "PUT etag=" << put_resp.gds_result.value().etag << "\n";

  // 5. StatObject
  std::uint64_t obj_size = 0;
  std::string stat_err;
  client.StatObject("test-bucket", "gds-get-demo", obj_size, stat_err);
  std::cout << "StatObject size=" << rtest::HumanBytes(obj_size) << "\n";

  // 6. GET（清零 dev_get 后读回）
  cudaMemset(dev_get, 0xAA, kSize);
  GetPathResult get_res;
  client.GetObjectGds("test-bucket", "gds-get-demo",
                      MutableBufferView{.data = dev_get, .size = obj_size}, get_res);
  std::cout << "GET bytes_read=" << get_res.bytes_read << "\n";

  // 7. D2H + 逐字节比对
  std::vector<std::byte> host_read(kSize);
  cudaMemcpy(host_read.data(), dev_get, kSize, cudaMemcpyDeviceToHost);
  bool ok = rtest::VerifyHostBuffer(host_read.data(), kSize, host, "get-demo");

  cudaFree(dev_put);
  cudaFree(dev_get);
  client.Shutdown();

  return ok ? 0 : 1;
}
