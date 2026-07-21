// rdma_multipart_example.cpp — RDMA 分段上传最简示例。
//
// 演示: CreateMultipartUpload → UploadPartRdma → CompleteMultipartUpload。
// 运行: us3_turbo_rdma_multipart_example [proxy_addr]

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

// ---- 本地常量 ----
constexpr const char* kTestProxy = "192.168.1.198:9100";
constexpr const char* kTestBucket = "test-bucket";
constexpr std::uint32_t kTestNumParts = 2;

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  const std::string proxy_addr = (argc > 1) ? argv[1] : kTestProxy;
  constexpr std::uint64_t kPartSize = rtest::kDefaultPartSize;
  constexpr std::uint32_t kNumParts = kTestNumParts;
  constexpr std::uint64_t kTotal = kPartSize * kNumParts;

  // 1. 准备整对象 host buffer（每 part 从中切片）
  std::vector<std::byte> host(kTotal);
  rtest::FillHostPattern(host);

  // 2. 初始化 client
  Client client(ClientOptions{.endpoint = proxy_addr});
  client.Initialize();

  // 3. 创建分段上传会话
  std::string upload_id, err;
  client.CreateMultipartUpload(kTestBucket, "rdma-mp-demo", PutDataPath::kRdma, upload_id, err);

  // 4. 上传每个 part（buffer 切片引用）
  std::vector<Client::PartInfo> parts;
  for (std::uint32_t i = 1; i <= kNumParts; ++i) {
    const std::uint64_t off = (i - 1) * kPartSize;
    std::string etag;
    client.UploadPartRdma(upload_id, i,
                          ConstBufferView{.data = host.data() + off, .size = kPartSize}, etag, err);
    parts.push_back({i, etag});
  }

  // 5. 完成分段上传
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
