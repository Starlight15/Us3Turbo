// test_get_multi_block_hash.cpp — T2.2 GET 多块对象 hash
//
// 验证: 20MB 对象经 multipart 上传 [16M,4M]（multipart 存 block_size=16MB =
// part_size → 2 块） 后 GET 读回，result.crc32c==0、hash 非空、bytes_read==20MB。
// 关键修正: single PUT 不可能传 20MB（>max_single_put_bytes=16MB 被拒；且 single
// PUT 存 block_size=filesize → 恒单块 → crc32c 永非 0）。故必须用 multipart。 PUT
// 无 hash 字段、公开 StatObject 不返回 hash，故无 hash_put/hash_get 直比。
// 失败条件: crc32c!=0、hash 空、bytes_read!=total。

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

namespace {
constexpr char kTestName[] = "gds_get_multi_block_hash";
constexpr std::uint64_t kPartSizeLimit = 16ULL * 1024 * 1024;
}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = "192.168.1.198:9100";
  std::uint64_t total = 20ULL * 1024 * 1024;  // 默认 20M（>16M 触发多块）

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
      if (!need(v) || !rtest::ParseSize(v, total)) {
        std::cerr << "bad --size\n";
        return 2;
      }
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  if (total <= kPartSizeLimit) {
    std::cerr << "[FAIL] " << kTestName
              << ": size must be > 16M for multi-block, got "
              << rtest::HumanBytes(total) << "\n";
    return 2;
  }
  const std::uint64_t part1 = kPartSizeLimit;  // 16M（非 last，须 == 上限）
  const std::uint64_t part2 = total - kPartSizeLimit;  // last（<= 上限）

  const std::string bucket = "test-bucket";
  const std::string key =
      std::string("rtest-t22-gds-") + rtest::MakeTimestampSuffix();

  std::cout << "=== T2.2 GDS " << kTestName << " ===\n"
            << "  proxy : " << proxy_addr << "\n"
            << "  total : " << rtest::HumanBytes(total) << " ("
            << rtest::HumanBytes(part1) << " + " << rtest::HumanBytes(part2)
            << ")\n";

  // 完整期望数据（各 part 不同 offset_base pattern）。
  std::vector<std::byte> host_full(total);
  {
    std::vector<std::byte> p1(part1);
    rtest::FillHostPattern(p1, 0);
    std::memcpy(host_full.data(), p1.data(), part1);
    std::vector<std::byte> p2(part2);
    rtest::FillHostPattern(p2, part1);
    std::memcpy(host_full.data() + part1, p2.data(), part2);
  }

  // dev_put 复用于两个 part；dev_get 独立，两者同时存活（避免 cuObj descriptor
  // 失效）。
  void* dev_put = nullptr;
  void* dev_get = nullptr;
  if (cudaError_t e = cudaMalloc(&dev_put, part1); e != cudaSuccess) {
    std::cerr << "[FAIL] " << kTestName
              << ": cudaMalloc(put): " << cudaGetErrorString(e) << "\n";
    return 1;
  }

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.verify_crc32c = false;
  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    cudaFree(dev_put);
    return 1;
  }

  bool test_passed = false;
  std::string fail_reason;

  // ---- CreateMultipartUpload ----
  std::string upload_id, error;
  if (!client.CreateMultipartUpload(bucket, key, PutDataPath::kGds, upload_id,
                                    error)) {
    fail_reason = "CreateMultipartUpload failed: " + error;
    goto cleanup;
  }

  // ---- Part 1 (16M) + Part 2 (last) + Complete（统一作用域保存
  // etag1/etag2）----
  {
    std::string etag1, etag2;
    // Part 1
    if (cudaError_t e = cudaMemcpy(dev_put, host_full.data(), part1,
                                   cudaMemcpyHostToDevice);
        e != cudaSuccess) {
      fail_reason = std::string("cudaMemcpy p1: ") + cudaGetErrorString(e);
      goto cleanup;
    }
    if (!client.UploadPartGds(upload_id, 1,
                              ConstBufferView{.data = dev_put, .size = part1},
                              etag1, error)) {
      fail_reason = "UploadPartGds 1 failed: " + error;
      goto cleanup;
    }
    // Part 2
    if (cudaError_t e = cudaMemcpy(dev_put, host_full.data() + part1, part2,
                                   cudaMemcpyHostToDevice);
        e != cudaSuccess) {
      fail_reason = std::string("cudaMemcpy p2: ") + cudaGetErrorString(e);
      goto cleanup;
    }
    if (!client.UploadPartGds(upload_id, 2,
                              ConstBufferView{.data = dev_put, .size = part2},
                              etag2, error)) {
      fail_reason = "UploadPartGds 2 failed: " + error;
      goto cleanup;
    }
    // Complete
    std::vector<Client::PartInfo> parts{{1, etag1}, {2, etag2}};
    Client::CompletedMultipart done;
    if (!client.CompleteMultipartUpload(upload_id, parts, done)) {
      fail_reason = "CompleteMultipartUpload failed: " + done.error;
      goto cleanup;
    }
    if (done.object_size != total) {
      fail_reason = "object_size mismatch: got " +
                    std::to_string(done.object_size) + " want " +
                    std::to_string(total);
      goto cleanup;
    }
    std::cout << "  CompleteMultipartUpload: object_size=" << done.object_size
              << " etag=" << done.etag << "\n";
  }

  // ---- StatObject + GET ----
  {
    std::uint64_t obj_size = 0;
    std::string stat_err;
    if (!client.StatObject(bucket, key, obj_size, stat_err) ||
        obj_size != total) {
      fail_reason = "StatObject failed or size mismatch";
      goto cleanup;
    }
    if (cudaError_t e = cudaMalloc(&dev_get, total); e != cudaSuccess) {
      fail_reason = std::string("cudaMalloc(get): ") + cudaGetErrorString(e);
      goto cleanup;
    }
    cudaMemset(dev_get, 0xBB, total);
    GetPathResult get_res;
    if (!client.GetObjectGds(bucket, key,
                             MutableBufferView{.data = dev_get, .size = total},
                             get_res) ||
        !get_res.ok) {
      fail_reason = "GetObjectGds FAILED: " + get_res.error_message;
      goto cleanup;
    }
    std::cout << "  GET OK: bytes_read=" << get_res.bytes_read << " crc32c=0x"
              << std::hex << get_res.crc32c << std::dec
              << " hash=" << get_res.hash << "\n";

    if (get_res.crc32c != 0) {
      fail_reason = "crc32c != 0 (expected 0 for multi-block)";
    } else if (get_res.hash.empty()) {
      fail_reason = "hash is empty";
    } else if (get_res.bytes_read != total) {
      fail_reason = "bytes_read mismatch: got " +
                    std::to_string(get_res.bytes_read) + " want " +
                    std::to_string(total);
    } else {
      test_passed = true;
    }

    // D2H + 逐字节比对（加分项）。
    std::vector<std::byte> host_read(total);
    cudaMemcpy(host_read.data(), dev_get, total, cudaMemcpyDeviceToHost);
    rtest::VerifyHostBuffer(host_read.data(), total, host_full, "multi-block");
  }

cleanup:
  client.Shutdown();
  if (dev_get) cudaFree(dev_get);
  cudaFree(dev_put);

  if (test_passed) {
    std::cout << "[PASS] " << kTestName << "\n";
    return 0;
  }
  std::cerr << "[FAIL] " << kTestName << ": " << fail_reason << "\n";
  return 1;
}
