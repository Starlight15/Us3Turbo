// gds_put_example.cpp — GDS 单步 PUT 端到端示例（rtest/examples/gds）。
//
// client 内部分配 cuObj token，proxy 通过 RDMA-READ 从 GPU 显存拉取数据
// 写入后端存储。演示最简单的 GDS PUT API 调用流程。
//
// 用法:
//   us3_turbo_gds_put_example --proxy 192.168.1.198:9100 [--size 4M]

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = "192.168.1.198:9100";
  std::uint64_t bytes = 4ULL * 1024 * 1024;  // 默认 4M（单步上限 16M 内）

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need = [&](std::string& v) -> bool {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << arg << "\n";
        return false;
      }
      v = argv[++i];
      return true;
    };
    if (arg == "--proxy") {
      if (!need(proxy_addr)) return 2;
    } else if (arg == "--size") {
      std::string v;
      if (!need(v) || !rtest::ParseSize(v, bytes)) {
        std::cerr << "bad --size\n";
        return 2;
      }
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: us3_turbo_gds_put_example [options]\n"
                << "  --proxy HOST:PORT   proxy endpoint (default 192.168.1.198:9100)\n"
                << "  --size N[K|M|G]     object size (default 4M, <=16M)\n";
      return 0;
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
  rtest::FillHostPattern(host);
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
  bool put_ok = client.PutObject(req, ConstBufferView{.data = dev, .size = bytes}, resp);

  cudaFree(dev);

  if (!put_ok) {
    std::cerr << "PutObject FAILED\n";
    return 1;
  }
  const auto& r = resp.gds_result.value();
  std::cout << "OK bytes=" << r.bytes_written << " etag=" << r.etag << "\n";
  return 0;
}
