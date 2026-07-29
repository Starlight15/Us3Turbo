// test_multipart_single_part.cpp — T1.3 单 part = 整对象。
//
// 验证: 单 part 分段上传 Complete 成功且 object_size==part_size。可选 GET 校验 hash。

#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  constexpr const char* kProxy = rtest::kDefaultProxyEndpoint;
  constexpr const char* kBucket = "test-bucket";
  constexpr char kTestName[] = "gds_multipart_single_part";

  std::string proxy_addr = kProxy;
  std::uint64_t part_size = rtest::kDefaultPartSize;  // 默认 4M（== proxy part 上限）
  const std::string bucket = kBucket;
  const std::string key = std::string("rtest-t13-gds-") + rtest::MakeTimestampSuffix();

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

  std::cout << "=== T1.3 GDS " << kTestName << " ===\n"
            << "  proxy     : " << proxy_addr << "\n"
            << "  part_size : " << rtest::HumanBytes(part_size) << "\n";

  void* dev_put = nullptr;
  cudaError_t e = cudaMalloc(&dev_put, part_size);
  if (e != cudaSuccess) {
    std::cerr << "[FAIL] " << kTestName << ": cudaMalloc: " << cudaGetErrorString(e) << "\n";
    return 1;
  }
  std::vector<std::byte> host(part_size);
  rtest::FillHostPattern(host);
  e = cudaMemcpy(dev_put, host.data(), part_size, cudaMemcpyHostToDevice);
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

  // ---- CreateMultipartUpload ----
  std::string upload_id, error;
  if (!client.CreateMultipartUpload(bucket, key, PutDataPath::kGds, upload_id, error)) {
    fail_reason = "CreateMultipartUpload failed: " + error;
    goto cleanup;
  }

  // ---- UploadPart (part 1) + Complete（同一作用域复用 etag）----
  {
    std::string etag;
    if (!client.UploadPartGds(upload_id, 1, ConstBufferView{.data = dev_put, .size = part_size},
                              etag, error)) {
      fail_reason = "UploadPartGds 1 failed: " + error;
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

  // ---- 可选 GET：验证 hash 非空 + bytes_read（不断言 crc32c!=0）----
  if (test_passed) {
    std::uint64_t obj_size = 0;
    std::string stat_err;
    if (!client.StatObject(bucket, key, obj_size, stat_err) || obj_size != part_size) {
      std::cout << "  (optional GET skipped: StatObject failed or size "
                   "mismatch)\n";
    } else if (cudaMalloc(&dev_get, obj_size) != cudaSuccess) {
      std::cout << "  (optional GET skipped: cudaMalloc(get) failed)\n";
    } else {
      cudaMemset(dev_get, 0xAA, obj_size);
      GetPathResult get_res;
      if (client.GetObjectGds(bucket, key, MutableBufferView{.data = dev_get, .size = obj_size},
                              get_res) &&
          get_res.ok) {
        std::cout << "  GET: bytes_read=" << get_res.bytes_read << " crc32c=0x" << std::hex
                  << get_res.crc32c << std::dec << " hash=" << get_res.hash << "\n";
        // 1 block/part: 单 part = 单块 → crc32c = 该块 crc（非 0）。
        if (!get_res.hash.empty() && get_res.bytes_read == obj_size) {
          std::cout << "  optional GET checks OK (hash non-empty)\n";
        } else {
          std::cout << "  optional GET warning: hash empty or bytes_read!=size"
                    << "\n";
        }
        // D2H + 逐字节比对（加分项）。
        std::vector<std::byte> host_read(obj_size);
        cudaMemcpy(host_read.data(), dev_get, obj_size, cudaMemcpyDeviceToHost);
        rtest::VerifyHostBuffer(host_read.data(), obj_size, host, "single-part");
      } else {
        std::cout << "  (optional GET failed: " << get_res.error_message << ")\n";
      }
    }
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
