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

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  const char* kProxy = (argc > 1) ? argv[1] : rtest::kDefaultProxyEndpoint;
  constexpr const char* kBucket = "test-bucket";
  constexpr std::uint64_t kPartSize = rtest::kDefaultPartSize;
  constexpr std::uint32_t kNumParts = 2;
  constexpr std::uint64_t kTotal = kPartSize * kNumParts;

  void* dev = nullptr;
  cudaMalloc(&dev, kPartSize);
  std::vector<std::byte> host(kPartSize);
  rtest::FillHostPattern(host);
  cudaMemcpy(dev, host.data(), kPartSize, cudaMemcpyHostToDevice);

  Client client(ClientOptions{.endpoint = kProxy});
  client.Initialize();

  std::string upload_id, err;
  client.CreateMultipartUpload(kBucket, "gds-mp-demo", PutDataPath::kGds, upload_id, err);

  std::vector<Client::PartInfo> parts;
  for (std::uint32_t i = 1; i <= kNumParts; ++i) {
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
