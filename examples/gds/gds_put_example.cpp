#include <cuda_runtime.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "us3_turbo/client/client.h"

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  const std::string proxy_addr = "192.168.1.198:9100";
  const std::string backend_addr = "192.168.1.198:9200";
  const std::size_t bytes = 100UL * 1024UL * 1024UL;
  const std::string bucket = "test-bucket";
  const std::string key = "obj1";

  // ---- 分配并初始化 GPU 内存 ----
  void* dev = nullptr;
  if (cudaError_t e = cudaMalloc(&dev, bytes); e != cudaSuccess) {
    std::cerr << "cudaMalloc: " << cudaGetErrorString(e) << "\n";
    return 1;
  }
  std::vector<std::byte> host(bytes);
  for (std::size_t i = 0; i < bytes; ++i) {
    host[i] = static_cast<std::byte>(i % 251U);
  }

  if (cudaError_t e = cudaMemcpy(dev, host.data(), bytes, cudaMemcpyHostToDevice);
      e != cudaSuccess) {
    std::cerr << "cudaMemcpy: " << cudaGetErrorString(e) << "\n";
    cudaFree(dev);
    return 1;
  }

  // ---- 初始化 Client ----
  // 控制面（OpenSession）→ proxy；数据面（GdsPut）→ backend，二者分离，
  // 故 gds_data_endpoint 指向 backend。两者都用 192.168.1.198，避免任何
  // 127.0.0.1 / 0.0.0.0。
  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.gds_data_endpoint = backend_addr;
  opts.client_id = "us3-gds-example";
  opts.data_flow = DataFlow::GPUDirect;

  Client client(std::move(opts));
  if (auto r = client.Initialize(); !r.success()) {
    std::cerr << "Initialize: " << r.error().message << "\n";
    cudaFree(dev);
    return 1;
  }

  // ---- 注册显存 ----
  if (auto r = client.RegisterDeviceBuffer(dev, bytes); !r.success()) {
    std::cerr << "RegisterDeviceBuffer: " << r.error().message << "\n";
    cudaFree(dev);
    return 1;
  }

  // ---- PUT ----
  PutObjectRequest req;
  req.set_bucket(bucket)
     .set_key(key)
     .set_expected_size(bytes);  // GDS PUT 必须指定 expected_size

  auto put = client.PutObject(
      req, ConstBufferView{.data = dev, .size = bytes, .type = BufferType::kCudaDevice});

  // ---- 反注册显存后释放 GPU 内存 ----
  (void)client.UnregisterDeviceBuffer(dev);
  cudaFree(dev);

  if (!put.success()) {
    std::cerr << "PutObject FAILED: " << put.error().message << "\n";
    return 1;
  }

  std::cout << "OK path=" << ToString(put.value().selected_flow)
            << " bytes=" << put.value().bytes_transferred
            << " etag=" << put.value().etag << "\n";
  return 0;
}
