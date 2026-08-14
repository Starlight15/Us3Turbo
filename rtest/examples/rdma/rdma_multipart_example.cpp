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

  std::string proxy = rtest::kDefaultProxyEndpoint;
  std::string rdma_bind_ip;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--proxy" && i + 1 < argc) {
      proxy = argv[++i];
    } else if (a == "--rdma-bind-ip" && i + 1 < argc) {
      rdma_bind_ip = argv[++i];
    } else {
      std::cerr << "usage: " << argv[0] << " [--proxy HOST:PORT] [--rdma-bind-ip IP]\n";
      return 2;
    }
  }
  constexpr const char* kBucket = "test-bucket";
  constexpr std::uint64_t kPartSize = rtest::kDefaultPartSize;
  constexpr std::uint32_t kNumParts = 2;
  constexpr std::uint64_t kTotal = kPartSize * kNumParts;

  std::vector<std::byte> host(kTotal);
  rtest::FillHostPattern(host);

  Client client(ClientOptions{.endpoint = proxy, .rdma_bind_ip = rdma_bind_ip});
  client.Initialize();

  std::string upload_id, trace_id, err;
  client.CreateMultipartUpload(kBucket, "rdma-mp-demo", PutDataPath::kRdma, upload_id, trace_id, err);

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
