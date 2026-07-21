// test_multipart_invalid_part_size.cpp — T1.1 part < 4M 在 Complete 时被拒。
//
// 验证: 中间 part 小于 multipart_part_size,Complete 返回 "invalid part size"。
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

namespace {
constexpr char kTestName[] = "gds_multipart_invalid_part_size";
constexpr const char* kTestProxy = "192.168.1.198:9100";
constexpr const char* kTestBucket = "test-bucket";
}

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = kTestProxy;
  std::uint64_t part_size = 3ULL * 1024 * 1024;  // 默认 3M（< 4M）

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
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  constexpr std::uint32_t num_parts = 3;
  const std::string bucket = kTestBucket;
  const std::string key = std::string("rtest-t11-gds-") + rtest::MakeTimestampSuffix();

  std::cout << "=== T1.1 GDS " << kTestName << " ===\n"
            << "  proxy     : " << proxy_addr << "\n"
            << "  part_size : " << rtest::HumanBytes(part_size) << "  num_parts : " << num_parts
            << "\n";

  // GPU buffer（3 个 part 复用同一 3MB buffer）。
  void* dev = nullptr;
  cudaError_t e = cudaMalloc(&dev, part_size);
  if (e != cudaSuccess) {
    std::cerr << "[FAIL] " << kTestName << ": cudaMalloc: " << cudaGetErrorString(e) << "\n";
    return 1;
  }
  std::vector<std::byte> host(part_size);
  rtest::FillHostPattern(host);
  e = cudaMemcpy(dev, host.data(), part_size, cudaMemcpyHostToDevice);
  if (e != cudaSuccess) {
    std::cerr << "[FAIL] " << kTestName << ": cudaMemcpy: " << cudaGetErrorString(e) << "\n";
    cudaFree(dev);
    return 1;
  }

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.verify_crc32c = false;
  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    cudaFree(dev);
    return 1;
  }

  bool test_passed = false;
  std::string fail_reason;

  // ---- CreateMultipartUpload ----
  std::string upload_id, error;
  if (!client.CreateMultipartUpload(bucket, key, PutDataPath::kGds, upload_id, error)) {
    fail_reason = "CreateMultipartUpload failed: " + error;
    goto cleanup;
  }
  std::cout << "  CreateMultipartUpload: upload_id=" << upload_id << "\n";

  // ---- UploadPart ×3（3MB < 4M，UploadPart 应全部接受）----
  {
    std::vector<Client::PartInfo> parts;
    parts.reserve(num_parts);
    for (std::uint32_t i = 1; i <= num_parts; ++i) {
      std::string etag;
      if (!client.UploadPartGds(upload_id, i, ConstBufferView{.data = dev, .size = part_size}, etag,
                                error)) {
        // 3MB 不应被 UploadPart 拒；若被拒说明 UploadPart 行为变了，记录之。
        std::cout << "  UploadPartGds " << i << " REJECTED (unexpected): " << error << "\n";
      } else {
        std::cout << "  UploadPartGds " << i << " ok etag=" << etag << "\n";
        parts.push_back({i, etag});
      }
    }

    // ---- Complete（期望失败 + "invalid part size"）----
    Client::CompletedMultipart done;
    const bool complete_ok = client.CompleteMultipartUpload(upload_id, parts, done);
    std::cout << "  CompleteMultipartUpload: " << (complete_ok ? "succeeded" : "FAILED (expected)")
              << " error=" << done.error << "\n";
    if (complete_ok) {
      fail_reason = "expected Complete to fail, but it succeeded (object_size=" +
                    std::to_string(done.object_size) + ")";
    } else if (done.error.find("invalid part size") == std::string::npos) {
      fail_reason = "Complete failed but error lacks \"invalid part size\": " + done.error;
    } else {
      test_passed = true;
    }
  }

cleanup:
  // Abort 幂等；failed-Complete 后 upload 仍在，清理之（忽略结果）。
  {
    std::string abort_err;
    (void)client.AbortMultipartUpload(upload_id, abort_err);
  }
  client.Shutdown();
  cudaFree(dev);

  if (test_passed) {
    std::cout << "[PASS] " << kTestName << "\n";
    return 0;
  }
  std::cerr << "[FAIL] " << kTestName << ": " << fail_reason << "\n";
  return 1;
}
