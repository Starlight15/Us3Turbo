// gds_get_example.cpp — GDS PUT + GET 读回验证。

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

  const char* kProxy = (argc > 1) ? argv[1] : rtest::kDefaultProxyEndpoint;
  constexpr const char* kBucket = "test-bucket";
  constexpr std::uint64_t kSize = 4ULL * 1024 * 1024;

  /* PUT buffer 和 GET buffer 同时存活，避免 cuObj descriptor 失效。 */
  void* dev_put = nullptr;
  void* dev_get = nullptr;
  cudaMalloc(&dev_put, kSize);
  cudaMalloc(&dev_get, kSize);

  std::vector<std::byte> host(kSize);
  rtest::FillHostPattern(host);
  cudaMemcpy(dev_put, host.data(), kSize, cudaMemcpyHostToDevice);

  Client client(ClientOptions{.endpoint = kProxy});
  client.Initialize();

  ClientProxyPutRequest put_req;
  put_req.bucket = kBucket;
  put_req.key = "gds-get-demo";
  put_req.object_size = kSize;
  put_req.path = PutDataPath::kGds;

  ClientProxyPutResponse put_resp;
  client.PutObjectGds(put_req, ConstBufferView{.data = dev_put, .size = kSize}, put_resp);
  std::cout << "PUT etag=" << put_resp.gds_result.value().etag << "\n";

  std::uint64_t obj_size = 0;
  std::string stat_err;
  client.StatObject(kBucket, "gds-get-demo", obj_size, stat_err);
  std::cout << "StatObject size=" << rtest::HumanBytes(obj_size) << "\n";

  cudaMemset(dev_get, 0xAA, kSize);
  GetPathResult get_res;
  client.GetObjectGds(kBucket, "gds-get-demo",
                      MutableBufferView{.data = dev_get, .size = obj_size}, get_res);
  std::cout << "GET bytes_read=" << get_res.bytes_read << "\n";

  std::vector<std::byte> host_read(kSize);
  cudaMemcpy(host_read.data(), dev_get, kSize, cudaMemcpyDeviceToHost);
  bool ok = rtest::VerifyHostBuffer(host_read.data(), kSize, host, "get-demo");

  cudaFree(dev_put);
  cudaFree(dev_get);
  client.Shutdown();
  return ok ? 0 : 1;
}
