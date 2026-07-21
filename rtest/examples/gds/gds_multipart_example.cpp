// gds_multipart_example.cpp — GDS 分段上传最简示例。
//
// 演示: CreateMultipartUpload → UploadPartGds → CompleteMultipartUpload。
// 运行: us3_turbo_gds_multipart_example [proxy_addr]

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
  constexpr std::uint64_t kPartSize = rtest::kDefaultPartSize;
  constexpr std::uint32_t kNumParts = rtest::kDefaultNumParts;
  constexpr std::uint64_t kTotal = kPartSize * kNumParts;

  // 1. 分配 GPU buffer + 填充测试数据
  void* dev = nullptr;
  cudaMalloc(&dev, kPartSize);
  std::vector<std::byte> host(kPartSize);
  rtest::FillHostPattern(host);
  cudaMemcpy(dev, host.data(), kPartSize, cudaMemcpyHostToDevice);

  // 2. 初始化 client
  Client client(ClientOptions{.endpoint = proxy_addr});
  client.Initialize();

  // 3. 创建分段上传会话
  std::string upload_id, err;
  client.CreateMultipartUpload(rtest::kDefaultBucket, "gds-mp-demo", PutDataPath::kGds, upload_id, err);

  // 4. 上传每个 part（复用同一 GPU buffer）
  std::vector<Client::PartInfo> parts;
  for (std::uint32_t i = 1; i <= kNumParts; ++i) {
    std::string etag;
    client.UploadPartGds(upload_id, i, ConstBufferView{.data = dev, .size = kPartSize}, etag, err);
    parts.push_back({i, etag});
  }

  // 5. 完成分段上传
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
