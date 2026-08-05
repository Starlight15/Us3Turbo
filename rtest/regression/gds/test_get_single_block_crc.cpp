// test_get_single_block_crc.cpp — GDS GET 单块对象 CRC 一致性。
//
// 验证: PUT 后 GET 读回，crc32c / hash / bytes_read 与 PUT 结果一致。

#include <cstdint>
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
  constexpr const char* kTestName = "gds_get_single_block_crc";
  constexpr std::uint64_t kSinglePutMax = 4ULL * 1024 * 1024;

  // ---- args ----
  std::string proxy = kProxy;
  std::uint64_t size = rtest::kDefaultPartSize / 2;  // 默认 2M（< 4M 单块）
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--proxy" && i + 1 < argc) {
      proxy = argv[++i];
    } else if (a == "--size" && i + 1 < argc) {
      if (!rtest::ParseSize(argv[++i], size)) {
        std::cerr << "bad --size\n";
        return 2;
      }
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      return 2;
    }
  }

  if (size > kSinglePutMax) {
    std::cerr << "[FAIL] " << kTestName << ": size must be <= 4M, got " << rtest::HumanBytes(size)
              << "\n";
    return 2;
  }

  std::cout << "=== " << kTestName << " ===\n"
            << "  proxy : " << proxy << "\n"
            << "  size  : " << rtest::HumanBytes(size) << "\n";

  // ---- GPU buffers ----
  rtest::DevMem dev_put(size);
  if (!dev_put.valid()) {
    std::cerr << "[FAIL] " << kTestName << ": cudaMalloc(put) failed\n";
    return 1;
  }
  rtest::DevMem dev_get;  // GET buffer，懒分配
  std::vector<std::byte> host(size);
  rtest::FillHostPattern(host);
  if (cudaMemcpy(dev_put.get(), host.data(), size, cudaMemcpyHostToDevice) != cudaSuccess) {
    std::cerr << "[FAIL] " << kTestName << ": cudaMemcpy failed\n";
    return 1;
  }

  // ---- init ----
  Client client(ClientOptions{.endpoint = proxy});
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }
  // RAII：作用域退出即 Shutdown（即使 early-return 也覆盖）。
  struct ShutdownGuard {
    Client* c;
    ~ShutdownGuard() { c->Shutdown(); }
  } shutdown_guard{&client};

  const std::string key = std::string("rtest-t21-gds-") + rtest::MakeTimestampSuffix();
  bool test_passed = false;

  // ---- PUT ----
  std::uint32_t put_crc = 0;
  std::string put_etag;
  {
    ClientProxyPutRequest req;
    req.bucket = kBucket;
    req.key = key;
    req.object_size = size;
    req.path = PutDataPath::kGds;

    ClientProxyPutResponse resp;
    if (!client.PutObjectGds(req, ConstBufferView{.data = dev_put.get(), .size = size}, resp)) {
      std::cerr << "[FAIL] " << kTestName << ": PutObjectGds returned false\n";
      return 1;
    }
    const auto& pr = resp.gds_result.value();
    put_etag = pr.etag;
    put_crc = pr.crc32c;
    std::cout << "  PUT: bytes=" << pr.bytes << " etag=" << put_etag << " crc32c=0x" << std::hex
              << put_crc << std::dec << "\n";
  }

  std::string trace_id;

  // ---- StatObject ----
  {
    std::uint64_t obj_size = 0;
    std::string stat_err;
    if (!client.StatObject(kBucket, key, obj_size, trace_id, stat_err) || obj_size != size) {
      std::cerr << "[FAIL] " << kTestName << ": StatObject failed or size mismatch\n";
      return 1;
    }
  }

  // ---- GET ----
  {
    if (!dev_get.alloc(size)) {
      std::cerr << "[FAIL] " << kTestName << ": cudaMalloc(get) failed\n";
      return 1;
    }
    cudaMemset(dev_get.get(), 0xAA, size);
    GetPathResult get_res;
    if (!client.GetObjectGds(kBucket, key, trace_id, MutableBufferView{.data = dev_get.get(), .size = size},
                             get_res) || !get_res.ok) {
      std::cerr << "[FAIL] " << kTestName << ": GetObjectGds: " << get_res.error_message << "\n";
      return 1;
    }
    std::cout << "  GET: bytes_read=" << get_res.bytes_read << " crc32c=0x" << std::hex
              << get_res.crc32c << std::dec << " hash=" << get_res.hash << "\n";

    if (get_res.crc32c == 0) {
      std::cerr << "[FAIL] " << kTestName << ": crc32c == 0 (expected non-zero)\n";
    } else if (get_res.hash.empty()) {
      std::cerr << "[FAIL] " << kTestName << ": hash is empty\n";
    } else if (get_res.crc32c != put_crc) {
      std::cerr << "[FAIL] " << kTestName << ": crc32c mismatch\n";
    } else if (get_res.hash != put_etag) {
      std::cerr << "[FAIL] " << kTestName << ": hash != put.etag\n";
    } else if (get_res.bytes_read != size) {
      std::cerr << "[FAIL] " << kTestName << ": bytes_read mismatch\n";
    } else {
      test_passed = true;
    }

    // D2H 逐字节比对
    std::vector<std::byte> host_read(size);
    cudaMemcpy(host_read.data(), dev_get.get(), size, cudaMemcpyDeviceToHost);
    rtest::VerifyHostBuffer(host_read.data(), size, host, "single-block");
  }

  if (test_passed) {
    std::cout << "[PASS] " << kTestName << "\n";
    return 0;
  }
  return 1;
}
