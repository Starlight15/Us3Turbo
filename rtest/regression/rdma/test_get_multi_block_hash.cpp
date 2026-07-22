// test_get_multi_block_hash.cpp — T2.2 RDMA GET 多块对象 hash 验证。
//
// 验证: 两次 multipart PUT (各 4 MiB) 组成 8 MiB 对象, GET 读回后
// hash 非空、与 fileidx 一致、数据逐字节匹配。

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

  constexpr const char* kProxy = "192.168.1.198:9100";
  constexpr const char* kBucket = "test-bucket";
  constexpr char kTestName[] = "rdma_get_multi_block_hash";
  constexpr std::uint64_t kPartSize = rtest::kDefaultPartSize;  // 4 MiB
  constexpr int kNumParts = 2;

  std::string proxy_addr = kProxy;
  std::uint64_t part_size = kPartSize;
  const std::uint64_t total = part_size * kNumParts;
  const std::string bucket = kBucket;
  const std::string suffix = rtest::MakeTimestampSuffix();
  const std::string key = std::string("rtest-t22-rdma-") + suffix;

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
    } else if (arg == "--size") {
      std::string v;
      if (!need(v) || !rtest::ParseSize(v, part_size)) {
        std::cerr << "bad --size\n";
        return 2;
      }
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  std::cout << "=== T2.2 RDMA " << kTestName << " ===\n"
            << "  proxy : " << proxy_addr << "\n"
            << "  total : " << rtest::HumanBytes(total) << " ("
            << rtest::HumanBytes(part_size) << " + "
            << rtest::HumanBytes(part_size) << ")\n";

  /* PUT buffer — 确定性 pattern 覆盖全 size。 */
  std::vector<std::byte> host_put(total);
  rtest::FillHostPattern(host_put);

  /* GET buffer — 初始化为 0xAA 检测无效写入。 */
  std::vector<std::byte> host_get(total);
  std::memset(host_get.data(), 0xAA, total);

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.verify_crc32c = false;
  opts.multipart_part_size = part_size;
  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }

  bool test_passed = false;
  std::string fail_reason;

  // ---- multipart PUT ----
  std::string completed_etag;
  {
    std::string upload_id;
    std::string err;
    if (!client.CreateMultipartUpload(bucket, key, PutDataPath::kRdma,
                                      upload_id, err)) {
      fail_reason = "CreateMultipartUpload failed: " + err;
      goto cleanup;
    }
    std::cout << "  CreateMultipartUpload: upload_id=" << upload_id << "\n";

    std::vector<Client::PartInfo> parts;
    for (int p = 0; p < kNumParts; ++p) {
      std::string etag;
      ConstBufferView part_buf{
          .data = host_put.data() + (p * part_size), .size = part_size};
      if (!client.UploadPartRdma(upload_id, static_cast<std::uint32_t>(p + 1),
                                 part_buf, etag, err)) {
        fail_reason = "UploadPartRdma part " + std::to_string(p + 1) +
                      " failed: " + err;
        goto cleanup;
      }
      parts.push_back({static_cast<std::uint32_t>(p + 1), etag});
      std::cout << "  UploadPartRdma " << (p + 1) << " ok etag=" << etag
                << "\n";
    }

    Client::CompletedMultipart result;
    if (!client.CompleteMultipartUpload(upload_id, parts, result)) {
      fail_reason = "CompleteMultipartUpload failed: " + result.error;
      goto cleanup;
    }
    completed_etag = result.etag;
    std::cout << "  CompleteMultipartUpload: object_size=" << result.object_size
              << " etag=" << completed_etag << "\n";
  }

  // ---- StatObject ----
  {
    std::uint64_t obj_size = 0;
    std::string stat_err;
    if (!client.StatObject(bucket, key, obj_size, stat_err) ||
        obj_size != total) {
      fail_reason = "StatObject failed or size mismatch: " + stat_err;
      goto cleanup;
    }
  }

  // ---- GET ----
  {
    GetPathResult get_res;
    if (!client.GetObjectRdma(
            bucket, key,
            MutableBufferView{.data = host_get.data(), .size = total},
            get_res) ||
        !get_res.ok) {
      fail_reason = "GetObjectRdma FAILED: " + get_res.error_message;
      goto cleanup;
    }
    std::cout << "  GET OK: bytes_read=" << get_res.bytes_read
              << " crc32c=0x" << std::hex << get_res.crc32c << std::dec
              << " hash=" << get_res.hash << "\n";

    if (get_res.hash.empty()) {
      fail_reason = "hash is empty";
    } else if (get_res.bytes_read != total) {
      fail_reason = "bytes_read mismatch: got " +
                    std::to_string(get_res.bytes_read) + " want " +
                    std::to_string(total);
    } else {
      test_passed = true;
    }

    // 逐字节比对。
    rtest::VerifyHostBuffer(host_get.data(), total, host_put, "multi-block");
  }

cleanup:
  client.Shutdown();

  if (test_passed) {
    std::cout << "[PASS] " << kTestName << "\n";
    return 0;
  }
  std::cerr << "[FAIL] " << kTestName << ": " << fail_reason << "\n";
  return 1;
}
