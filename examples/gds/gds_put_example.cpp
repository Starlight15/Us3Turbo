#include <cuda_runtime.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "us3_turbo/client/client.h"

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  const std::string proxy_addr = "192.168.1.198:9100";
  const std::size_t bytes      = 100UL * 1024UL * 1024UL;

  void* dev = nullptr;
  if (cudaError_t e = cudaMalloc(&dev, bytes); e != cudaSuccess) {
    std::cerr << "cudaMalloc: " << cudaGetErrorString(e) << "\n";
    return 1;
  }
  std::vector<std::byte> host(bytes);
  for (std::size_t i = 0; i < bytes; ++i) host[i] = static_cast<std::byte>(i % 251U);
  if (cudaError_t e = cudaMemcpy(dev, host.data(), bytes, cudaMemcpyHostToDevice); e != cudaSuccess) {
    std::cerr << "cudaMemcpy: " << cudaGetErrorString(e) << "\n";
    cudaFree(dev);
    return 1;
  }

  ClientOptions opts;
  opts.endpoint = proxy_addr;

  Client client(std::move(opts));
  if (!client.Initialize()) { std::cerr << "Initialize failed\n"; cudaFree(dev); return 1; }
  if (!client.RegisterDeviceBuffer(dev, bytes)) { std::cerr << "RegisterDeviceBuffer failed\n"; cudaFree(dev); return 1; }

  PutObjectRequest req;
  req.set_bucket("test-bucket").set_key("obj1").set_expected_size(bytes);

  TransferOutcome outcome;
  bool put_ok = client.PutObject(req, ConstBufferView{.data = dev, .size = bytes}, outcome);

  (void)client.UnregisterDeviceBuffer(dev);
  cudaFree(dev);

  if (!put_ok) { std::cerr << "PutObject FAILED\n"; return 1; }
  std::cout << "OK bytes=" << outcome.bytes_transferred << " etag=" << outcome.etag << "\n";
  return 0;
}
