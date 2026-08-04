// test_multipart_invalid_part_size.cpp — GDS part < 4M 在 Complete 时被拒。
//
// 验证: 用 3M part 上传 3 个 part，Complete 应返回 "invalid part size"。

#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "rtest/cuda_guard.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  // ---- 常量 ----
  constexpr const char* kProxy = rtest::kDefaultProxyEndpoint;
  constexpr const char* kBucket = "test-bucket";
  constexpr const char* kTestName = "gds_multipart_invalid_part_size";
  constexpr std::uint32_t kNumParts = 3;

  // ---- args ----
  std::string proxy = kProxy;
  std::uint64_t part_size = 3ULL * 1024 * 1024;  // 3M < 4M
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--proxy" && i + 1 < argc) {
      proxy = argv[++i];
    } else if (a == "--part-size" && i + 1 < argc) {
      if (!rtest::ParseSize(argv[++i], part_size)) {
        std::cerr << "bad --part-size\n";
        return 2;
      }
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      return 2;
    }
  }

  std::cout << "=== " << kTestName << " ===\n"
            << "  proxy     : " << proxy << "\n"
            << "  part_size : " << rtest::HumanBytes(part_size) << "  num_parts : " << kNumParts
            << "\n";

  // ---- GPU buffer ----
  rtest::DevMem dev(part_size);
  if (!dev.valid()) {
    std::cerr << "[FAIL] " << kTestName << ": cudaMalloc failed\n";
    return 1;
  }
  std::vector<std::byte> host(part_size);
  rtest::FillHostPattern(host);
  if (cudaMemcpy(dev.get(), host.data(), part_size, cudaMemcpyHostToDevice) != cudaSuccess) {
    std::cerr << "[FAIL] " << kTestName << ": cudaMemcpy failed\n";
    return 1;
  }

  // ---- init ----
  Client client(ClientOptions{.endpoint = proxy});
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }

  // ---- CreateMultipartUpload ----
  const std::string key = std::string("rtest-t11-gds-") + rtest::MakeTimestampSuffix();
  std::string upload_id, error;
  if (!client.CreateMultipartUpload(kBucket, key, PutDataPath::kGds, upload_id, error)) {
    std::cerr << "[FAIL] " << kTestName << ": CreateMultipartUpload: " << error << "\n";
    client.Shutdown();
    return 1;
  }
  std::cout << "  CreateMultipartUpload: upload_id=" << upload_id << "\n";

  // ---- UploadPart ×3 + Complete (期望失败) ----
  bool test_passed = false;
  {
    std::vector<Client::PartInfo> parts;
    for (std::uint32_t i = 1; i <= kNumParts; ++i) {
      std::string etag;
      if (client.UploadPartGds(upload_id, i, ConstBufferView{.data = dev.get(), .size = part_size},
                               etag, error)) {
        std::cout << "  UploadPartGds " << i << " ok etag=" << etag << "\n";
        parts.push_back({i, etag});
      } else {
        std::cout << "  UploadPartGds " << i << " REJECTED (unexpected): " << error << "\n";
      }
    }

    Client::CompletedMultipart done;
    const bool ok = client.CompleteMultipartUpload(upload_id, parts, done);
    std::cout << "  Complete: " << (ok ? "succeeded" : "FAILED (expected)")
              << " error=" << done.error << "\n";

    if (ok) {
      std::cerr << "[FAIL] " << kTestName << ": expected Complete to fail\n";
    } else if (done.error.find("invalid part size") == std::string::npos) {
      std::cerr << "[FAIL] " << kTestName << ": error lacks \"invalid part size\": " << done.error
                << "\n";
    } else {
      test_passed = true;
    }
  }

  // ---- cleanup ----
  {
    std::string abort_err;
    client.AbortMultipartUpload(upload_id, abort_err);
  }
  client.Shutdown();

  if (test_passed) {
    std::cout << "[PASS] " << kTestName << "\n";
    return 0;
  }
  return 1;
}
