// rdma_multipart_example.cpp — RDMA 分段上传最简示例。

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  const char* kProxy = (argc > 1) ? argv[1] : "192.168.1.198:9100";
  constexpr const char* kBucket = "test-bucket";
  constexpr std::uint64_t kPartSize = rtest::kDefaultPartSize;
  constexpr std::uint32_t kNumParts = 2;
  constexpr std::uint64_t kTotal = kPartSize * kNumParts;

  std::vector<std::byte> host(kTotal);
  rtest::FillHostPattern(host);

  Client client(ClientOptions{.endpoint = kProxy});
  client.Initialize();

  std::string upload_id, err;
  client.CreateMultipartUpload(kBucket, "rdma-mp-demo", PutDataPath::kRdma, upload_id, err);

  std::vector<Client::PartInfo> parts;
  for (std::uint32_t i = 1; i <= kNumParts; ++i) {
    const std::uint64_t off = (i - 1) * kPartSize;
    std::string etag;
    client.UploadPartRdma(upload_id, i,
                          ConstBufferView{.data = host.data() + off, .size = kPartSize}, etag, err);
    parts.push_back({i, etag});
  }

  Client::CompletedMultipart done;
  client.CompleteMultipartUpload(upload_id, parts, done);
  client.Shutdown();

  if (done.object_size == kTotal) {
    std::cout << "OK object_id=" << done.object_id << " etag=" << done.etag
              << " size=" << done.object_size << "\n";
    return 0;
  }
  std::cerr << "FAILED: " << done.error << "\n";
  return 1;
}
