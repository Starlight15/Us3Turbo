// gds_multipart_example.cpp — GDS 分段上传最简示例。

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

constexpr const char* kTestProxy = "192.168.1.198:9100";
constexpr const char* kTestBucket = "test-bucket";
constexpr std::uint32_t kTestNumParts = 2;

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  const std::string proxy_addr = (argc > 1) ? argv[1] : kTestProxy;
  constexpr std::uint64_t kPartSize = rtest::kDefaultPartSize;
  constexpr std::uint64_t kTotal = kPartSize * kTestNumParts;

  void* dev = nullptr;
  cudaMalloc(&dev, kPartSize);
  std::vector<std::byte> host(kPartSize);
  rtest::FillHostPattern(host);
  cudaMemcpy(dev, host.data(), kPartSize, cudaMemcpyHostToDevice);

  Client client(ClientOptions{.endpoint = proxy_addr});
  client.Initialize();

  std::string upload_id, err;
  client.CreateMultipartUpload(kTestBucket, "gds-mp-demo", PutDataPath::kGds, upload_id, err);

  std::vector<Client::PartInfo> parts;
  for (std::uint32_t i = 1; i <= kTestNumParts; ++i) {
    std::string etag;
    client.UploadPartGds(upload_id, i, ConstBufferView{.data = dev, .size = kPartSize}, etag, err);
    parts.push_back({i, etag});
  }

  Client::CompletedMultipart done;
  client.CompleteMultipartUpload(upload_id, parts, done);

  cudaFree(dev);
  client.Shutdown();

  if (done.object_size == kTotal) {
    std::cout << "OK object_id=" << done.object_id << " etag=" << done.etag
              << " size=" << done.object_size << "\n";
    return 0;
  }
  std::cerr << "FAILED: " << done.error << "\n";
  return 1;
}
