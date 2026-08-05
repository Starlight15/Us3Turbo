// test_multipart_single_part.cpp — RDMA 分段上传，单 part = 整对象。
//
// 验证: 单 part Complete 成功，object_size == part_size。host 内存，无 CUDA 依赖。

#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  // ---- 常量 ----
  constexpr const char* kProxy = rtest::kDefaultProxyEndpoint;
  constexpr const char* kBucket = "test-bucket";
  constexpr const char* kTestName = "rdma_multipart_single_part";

  // ---- args ----
  std::string proxy = kProxy;
  std::uint64_t part_size = rtest::kDefaultPartSize;
  bool verify = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--proxy" && i + 1 < argc) {
      proxy = argv[++i];
    } else if (a == "--part-size" && i + 1 < argc) {
      if (!rtest::ParseSize(argv[++i], part_size)) {
        std::cerr << "bad --part-size\n";
        return 2;
      }
    } else if (a == "--verify-crc32c") {
      verify = true;
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      return 2;
    }
  }

  std::cout << "=== " << kTestName << " ===\n"
            << "  proxy     : " << proxy << "\n"
            << "  part_size : " << rtest::HumanBytes(part_size) << "\n"
            << "  verify    : " << (verify ? "on" : "off") << "\n";

  // ---- host buffer ----
  std::vector<std::byte> host(part_size);
  rtest::FillHostPattern(host);

  // ---- init ----
  Client client(ClientOptions{.endpoint = proxy, .verify_crc32c = verify});
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }

  // ---- CreateMultipartUpload ----
  const std::string key = std::string("rtest-t13-rdma-") + rtest::MakeTimestampSuffix();
  std::string upload_id, trace_id, error;
  if (!client.CreateMultipartUpload(kBucket, key, PutDataPath::kRdma, upload_id, trace_id, error)) {
    std::cerr << "[FAIL] " << kTestName << ": CreateMultipartUpload: " << error << "\n";
    client.Shutdown();
    return 1;
  }

  // ---- UploadPart + Complete ----
  bool test_passed = false;
  {
    std::string etag;
    if (!client.UploadPartRdma(upload_id, trace_id, 1,
                               ConstBufferView{.data = host.data(), .size = part_size}, etag,
                               error)) {
      std::cerr << "[FAIL] " << kTestName << ": UploadPartRdma: " << error << "\n";
    } else {
      std::vector<Client::PartInfo> parts{{1, etag}};
      Client::CompletedMultipart done;
      if (!client.CompleteMultipartUpload(upload_id, trace_id, parts, done)) {
        std::cerr << "[FAIL] " << kTestName << ": CompleteMultipartUpload: " << done.error << "\n";
      } else {
        std::cout << "  Complete: object_size=" << done.object_size << " etag=" << done.etag << "\n";
        test_passed = (done.object_size == part_size);
        if (!test_passed) {
          std::cerr << "[FAIL] " << kTestName << ": object_size mismatch: got=" << done.object_size
                    << " want=" << part_size << "\n";
        }
      }
    }
  }

  // ---- cleanup ----
  {
    std::string abort_err;
    client.AbortMultipartUpload(upload_id, trace_id, abort_err);
  }
  client.Shutdown();

  if (test_passed) {
    std::cout << "[PASS] " << kTestName << "\n";
    return 0;
  }
  return 1;
}
