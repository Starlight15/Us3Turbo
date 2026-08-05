// test_get_multi_block_hash.cpp — RDMA GET 多块对象 hash 验证。
//
// 验证: 两次 multipart PUT (各 4 MiB) 组成 8 MiB 对象，GET 读回后
// hash 非空、逐字节匹配。host 内存，无 CUDA 依赖。

#include <cstdint>
#include <cstring>
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
  constexpr const char* kTestName = "rdma_get_multi_block_hash";
  constexpr std::uint64_t kPartSize = rtest::kDefaultPartSize;
  constexpr int kNumParts = 2;

  // ---- args ----
  std::string proxy = kProxy;
  std::uint64_t part_size = kPartSize;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--proxy" && i + 1 < argc) {
      proxy = argv[++i];
    } else if (a == "--size" && i + 1 < argc) {
      if (!rtest::ParseSize(argv[++i], part_size)) {
        std::cerr << "bad --size\n";
        return 2;
      }
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      return 2;
    }
  }

  const std::uint64_t total = part_size * kNumParts;
  std::cout << "=== " << kTestName << " ===\n"
            << "  proxy : " << proxy << "\n"
            << "  total : " << rtest::HumanBytes(total) << " (" << rtest::HumanBytes(part_size)
            << " × " << kNumParts << ")\n";

  // ---- buffers ----
  std::vector<std::byte> host_put(total);
  rtest::FillHostPattern(host_put);

  std::vector<std::byte> host_get(total);
  std::memset(host_get.data(), 0xAA, total);

  // ---- init ----
  Client client(ClientOptions{.endpoint = proxy, .multipart_part_size = part_size});
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }

  const std::string key = std::string("rtest-t22-rdma-") + rtest::MakeTimestampSuffix();
  bool test_passed = false;

  // ---- multipart PUT ----
  std::string completed_etag;
  {
    std::string upload_id, trace_id, err;
    if (!client.CreateMultipartUpload(kBucket, key, PutDataPath::kRdma, upload_id, trace_id, err)) {
      std::cerr << "[FAIL] " << kTestName << ": CreateMultipartUpload: " << err << "\n";
      goto cleanup;
    }
    std::cout << "  CreateMultipartUpload: upload_id=" << upload_id << "\n";

    std::vector<Client::PartInfo> parts;
    for (int p = 0; p < kNumParts; ++p) {
      std::string etag;
      ConstBufferView part_buf{.data = host_put.data() + (p * part_size), .size = part_size};
      if (!client.UploadPartRdma(upload_id, trace_id, static_cast<std::uint32_t>(p + 1), part_buf, etag,
                                 err)) {
        std::cerr << "[FAIL] " << kTestName << ": UploadPartRdma " << (p + 1) << ": " << err
                  << "\n";
        goto cleanup;
      }
      parts.push_back({static_cast<std::uint32_t>(p + 1), etag});
      std::cout << "  UploadPartRdma " << (p + 1) << " ok etag=" << etag << "\n";
    }

    Client::CompletedMultipart result;
    if (!client.CompleteMultipartUpload(upload_id, trace_id, parts, result)) {
      std::cerr << "[FAIL] " << kTestName << ": Complete: " << result.error << "\n";
      goto cleanup;
    }
    completed_etag = result.etag;
    std::cout << "  Complete: object_size=" << result.object_size << " etag=" << completed_etag
              << "\n";
  }

  // ---- StatObject + GET ----
  {
    std::uint64_t obj_size = 0;
    std::string get_trace_id, stat_err;
    if (!client.StatObject(kBucket, key, obj_size, get_trace_id, stat_err) || obj_size != total) {
      std::cerr << "[FAIL] " << kTestName << ": StatObject failed or size mismatch\n";
      goto cleanup;
    }

    GetPathResult get_res;
    if (!client.GetObjectRdma(kBucket, key, get_trace_id,
                              MutableBufferView{.data = host_get.data(), .size = total}, get_res) ||
        !get_res.ok) {
      std::cerr << "[FAIL] " << kTestName << ": GetObjectRdma: " << get_res.error_message << "\n";
      goto cleanup;
    }
    std::cout << "  GET: bytes_read=" << get_res.bytes_read << " crc32c=0x" << std::hex
              << get_res.crc32c << std::dec << " hash=" << get_res.hash << "\n";

    if (get_res.hash.empty()) {
      std::cerr << "[FAIL] " << kTestName << ": hash is empty\n";
    } else if (get_res.bytes_read != total) {
      std::cerr << "[FAIL] " << kTestName << ": bytes_read mismatch\n";
    } else {
      test_passed = true;
    }

    rtest::VerifyHostBuffer(host_get.data(), total, host_put, "multi-block");
  }

cleanup:
  client.Shutdown();

  if (test_passed) {
    std::cout << "[PASS] " << kTestName << "\n";
    return 0;
  }
  return 1;
}
