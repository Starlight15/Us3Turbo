// test_multipart_single_part.cpp — T1.3 单 part = 整对象。
//
// 验证: 单 part RDMA 分段上传 Complete 成功且 object_size==part_size。

#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  constexpr const char* kProxy = rtest::kDefaultProxyEndpoint;
  constexpr const char* kBucket = "test-bucket";
  constexpr char kTestName[] = "rdma_multipart_single_part";

  std::string proxy_addr = kProxy;
  std::uint64_t part_size = rtest::kDefaultPartSize;  // 默认 4M（== proxy part 上限）
  bool verify = false;
  const std::string bucket = kBucket;
  const std::string key = std::string("rtest-t13-rdma-") + rtest::MakeTimestampSuffix();

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
    } else if (arg == "--part-size") {
      std::string v;
      if (!need(v) || !rtest::ParseSize(v, part_size)) {
        std::cerr << "bad --part-size\n";
        return 2;
      }
    } else if (arg == "--verify-crc32c") {
      verify = true;
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  std::cout << "=== T1.3 RDMA " << kTestName << " ===\n"
            << "  proxy     : " << proxy_addr << "\n"
            << "  part_size : " << rtest::HumanBytes(part_size) << "\n"
            << "  verify    : " << (verify ? "on" : "off") << "\n";

  // host buffer。
  std::vector<std::byte> host(part_size);
  rtest::FillHostPattern(host);

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.verify_crc32c = verify;
  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }

  bool test_passed = false;
  std::string fail_reason;

  // ---- CreateMultipartUpload ----
  std::string upload_id, error;
  if (!client.CreateMultipartUpload(bucket, key, PutDataPath::kRdma, upload_id, error)) {
    fail_reason = "CreateMultipartUpload failed: " + error;
    goto cleanup;
  }

  // ---- UploadPart (part 1) + Complete ----
  {
    std::string etag;
    if (!client.UploadPartRdma(upload_id, 1,
                               ConstBufferView{.data = host.data(), .size = part_size}, etag,
                               error)) {
      fail_reason = "UploadPartRdma 1 failed: " + error;
      goto cleanup;
    }
    std::vector<Client::PartInfo> parts{{1, etag}};
    Client::CompletedMultipart done;
    if (!client.CompleteMultipartUpload(upload_id, parts, done)) {
      fail_reason = "CompleteMultipartUpload failed: " + done.error;
      goto cleanup;
    }
    std::cout << "  CompleteMultipartUpload: object_size=" << done.object_size
              << " etag=" << done.etag << "\n";
    if (done.object_size != part_size) {
      fail_reason = "object_size mismatch: got " + std::to_string(done.object_size) + " want " +
                    std::to_string(part_size);
      goto cleanup;
    }
    test_passed = true;
  }

cleanup:
  // Abort 幂等。
  {
    std::string abort_err;
    (void)client.AbortMultipartUpload(upload_id, abort_err);
  }
  client.Shutdown();

  if (test_passed) {
    std::cout << "[PASS] " << kTestName << "\n";
    return 0;
  }
  std::cerr << "[FAIL] " << kTestName << ": " << fail_reason << "\n";
  return 1;
}
