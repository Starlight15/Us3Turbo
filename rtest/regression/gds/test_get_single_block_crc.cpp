// test_get_single_block_crc.cpp — T2.1 GET 单块对象 crc32c
//
// 验证: single PUT 一个 2MB 对象（< max_single_put_bytes=16MB → 单块）后 GET
// 读回， result.crc32c 非零、hash 非空，且
// get.crc32c==put.crc32c、get.hash==put.etag （single PUT 存
// block_size=filesize → GET 恒单块 → crc32c=crcs[0]、
// hash=Crc32cToETag(crc)==put.etag）。
// 失败条件: crc32c==0、hash 空、get/put crc 或 hash 不一致、bytes_read!=size。

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

namespace {
constexpr char kTestName[] = "gds_get_single_block_crc";
constexpr std::uint64_t kSinglePutLimit = rtest::kSinglePutMaxBytes;
constexpr std::uint64_t kBlockSize = rtest::kDefaultPartSize;
}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = rtest::kDefaultProxy;
  std::uint64_t size = rtest::kDefaultPartSize / 2;  // 默认 2M（< 4M 单块，≤16M 单步上限）

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

  if (size > kSinglePutLimit || size >= kBlockSize) {
    std::cerr << "[FAIL] " << kTestName << ": size must be <= 16M and < 4M for single-block, got "
              << rtest::HumanBytes(size) << "\n";
    return 2;
  }

  const std::string bucket = rtest::kDefaultBucket;
  const std::string key = std::string("rtest-t21-gds-") + rtest::MakeTimestampSuffix();

  std::cout << "=== T2.1 GDS " << kTestName << " ===\n"
            << "  proxy : " << proxy_addr << "\n"
            << "  size  : " << rtest::HumanBytes(size) << "\n";

  void* dev_put = nullptr;
  cudaError_t e = cudaMalloc(&dev_put, size);
  if (e != cudaSuccess) {
    std::cerr << "[FAIL] " << kTestName << ": cudaMalloc(put): " << cudaGetErrorString(e) << "\n";
    return 1;
  }
  std::vector<std::byte> host(size);
  rtest::FillHostPattern(host);
  e = cudaMemcpy(dev_put, host.data(), size, cudaMemcpyHostToDevice);
  if (e != cudaSuccess) {
    std::cerr << "[FAIL] " << kTestName << ": cudaMemcpy: " << cudaGetErrorString(e) << "\n";
    cudaFree(dev_put);
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
  void* dev_get = nullptr;

  // ---- single PUT ----
  std::uint32_t put_crc = 0;
  std::string put_etag;
  {
    ClientProxyPutRequest put_req;
    put_req.bucket = bucket;
    put_req.key = key;
    put_req.object_size = size;
    put_req.path = PutDataPath::kGds;

    ClientProxyPutResponse put_resp;
    if (!client.PutObjectGds(put_req, ConstBufferView{.data = dev_put, .size = size}, put_resp)) {
      fail_reason = "PutObject FAILED";
      goto cleanup;
    }
    const auto& pr = put_resp.gds_result.value();
    put_etag = pr.etag;
    put_crc = pr.crc32c;
    std::cout << "  PUT OK: bytes=" << pr.bytes_written << " etag=" << put_etag << " crc32c=0x"
              << std::hex << put_crc << std::dec << "\n";
  }

  // ---- StatObject ----
  {
    std::uint64_t obj_size = 0;
    std::string stat_err;
    if (!client.StatObject(bucket, key, obj_size, stat_err) || obj_size != size) {
      fail_reason = "StatObject failed or size mismatch";
      goto cleanup;
    }
  }

  // ---- GET（单独分配 dev_get，保持 dev_put 存活，避免 cuObj descriptor
  // 失效）----
  {
    e = cudaMalloc(&dev_get, size);
    if (e != cudaSuccess) {
      fail_reason = std::string("cudaMalloc(get): ") + cudaGetErrorString(e);
      goto cleanup;
    }
    cudaMemset(dev_get, 0xAA, size);
    GetPathResult get_res;
    if (!client.GetObjectGds(bucket, key, MutableBufferView{.data = dev_get, .size = size},
                             get_res) ||
        !get_res.ok) {
      fail_reason = "GetObjectGds FAILED: " + get_res.error_message;
      goto cleanup;
    }
    std::cout << "  GET OK: bytes_read=" << get_res.bytes_read << " crc32c=0x" << std::hex
              << get_res.crc32c << std::dec << " hash=" << get_res.hash << "\n";

    if (get_res.crc32c == 0) {
      fail_reason = "crc32c == 0 (expected non-zero for single block)";
    } else if (get_res.hash.empty()) {
      fail_reason = "hash is empty";
    } else if (get_res.crc32c != put_crc) {
      fail_reason = "crc32c mismatch: get=0x" + std::to_string(get_res.crc32c) + " put=0x" +
                    std::to_string(put_crc);
    } else if (get_res.hash != put_etag) {
      fail_reason = "hash != put.etag: get=" + get_res.hash + " put=" + put_etag;
    } else if (get_res.bytes_read != size) {
      fail_reason = "bytes_read mismatch: got " + std::to_string(get_res.bytes_read) + " want " +
                    std::to_string(size);
    } else {
      test_passed = true;
    }

    // D2H + 逐字节比对（加分项）。
    std::vector<std::byte> host_read(size);
    cudaMemcpy(host_read.data(), dev_get, size, cudaMemcpyDeviceToHost);
    rtest::VerifyHostBuffer(host_read.data(), size, host, "single-block");
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
