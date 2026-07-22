// test_get_single_block_crc.cpp — T2.1 RDMA GET 单块对象 CRC 一致性。
//
// 验证: single PUT 后 GET 读回, crc32c/hash/bytes_read 与 PUT 结果一致。
// host 内存路径, 无 CUDA 依赖。

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
  constexpr char kTestName[] = "rdma_get_single_block_crc";
  constexpr std::uint64_t kSinglePutMax = 4ULL * 1024 * 1024;

  std::string proxy_addr = kProxy;
  std::uint64_t size = rtest::kDefaultPartSize / 2;  // 默认 2M (< 4M 单块上限)
  const std::string bucket = kBucket;
  const std::string key =
      std::string("rtest-t21-rdma-") + rtest::MakeTimestampSuffix();

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
      if (!need(v) || !rtest::ParseSize(v, size)) {
        std::cerr << "bad --size\n";
        return 2;
      }
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  if (size > kSinglePutMax) {
    std::cerr << "[FAIL] " << kTestName
              << ": size must be <= 4M for single-block, got "
              << rtest::HumanBytes(size) << "\n";
    return 2;
  }

  std::cout << "=== T2.1 RDMA " << kTestName << " ===\n"
            << "  proxy : " << proxy_addr << "\n"
            << "  size  : " << rtest::HumanBytes(size) << "\n";

  /* PUT buffer — 确定性 pattern。 */
  std::vector<std::byte> host_put(size);
  rtest::FillHostPattern(host_put);

  /* GET buffer — 初始化为 0xAA 检测无效写入。 */
  std::vector<std::byte> host_get(size);
  std::memset(host_get.data(), 0xAA, size);

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.verify_crc32c = false;
  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }

  bool test_passed = false;
  std::string fail_reason;

  // ---- single PUT ----
  std::uint32_t put_crc = 0;
  std::string put_etag;
  {
    ClientProxyPutRequest put_req;
    put_req.bucket = bucket;
    put_req.key = key;
    put_req.object_size = size;
    put_req.path = PutDataPath::kRdma;

    ClientProxyPutResponse put_resp;
    if (!client.PutObjectRdma(
            put_req,
            ConstBufferView{.data = host_put.data(), .size = size},
            put_resp)) {
      fail_reason = "PutObjectRdma FAILED";
      goto cleanup;
    }
    const auto& pr = put_resp.rdma_result.value();
    put_etag = pr.etag;
    put_crc = pr.crc32c;
    std::cout << "  PUT OK: bytes=" << pr.bytes_written << " etag=" << put_etag
              << " crc32c=0x" << std::hex << put_crc << std::dec << "\n";
  }

  // ---- StatObject ----
  {
    std::uint64_t obj_size = 0;
    std::string stat_err;
    if (!client.StatObject(bucket, key, obj_size, stat_err) ||
        obj_size != size) {
      fail_reason = "StatObject failed or size mismatch";
      goto cleanup;
    }
  }

  // ---- GET ----
  {
    GetPathResult get_res;
    if (!client.GetObjectRdma(
            bucket, key,
            MutableBufferView{.data = host_get.data(), .size = size},
            get_res) ||
        !get_res.ok) {
      fail_reason = "GetObjectRdma FAILED: " + get_res.error_message;
      goto cleanup;
    }
    std::cout << "  GET OK: bytes_read=" << get_res.bytes_read
              << " crc32c=0x" << std::hex << get_res.crc32c << std::dec
              << " hash=" << get_res.hash << "\n";

    if (get_res.crc32c == 0) {
      fail_reason = "crc32c == 0 (expected non-zero for single block)";
    } else if (get_res.hash.empty()) {
      fail_reason = "hash is empty";
    } else if (get_res.crc32c != put_crc) {
      fail_reason = "crc32c mismatch: get=0x" +
                    std::to_string(get_res.crc32c) + " put=0x" +
                    std::to_string(put_crc);
    } else if (get_res.hash != put_etag) {
      fail_reason =
          "hash != put.etag: get=" + get_res.hash + " put=" + put_etag;
    } else if (get_res.bytes_read != size) {
      fail_reason = "bytes_read mismatch: got " +
                    std::to_string(get_res.bytes_read) + " want " +
                    std::to_string(size);
    } else {
      test_passed = true;
    }

    // 逐字节比对。
    rtest::VerifyHostBuffer(host_get.data(), size, host_put, "single-block");
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
