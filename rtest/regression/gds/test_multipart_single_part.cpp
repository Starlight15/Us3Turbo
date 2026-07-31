// test_multipart_single_part.cpp — GDS 分段上传，单 part = 整对象。
//
// 验证: 单 part Complete 成功，object_size == part_size，可选 GET 校验。

#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  // ---- 常量 ----
  constexpr const char* kProxy = rtest::kDefaultProxyEndpoint;
  constexpr const char* kBucket = "test-bucket";
  constexpr const char* kTestName = "gds_multipart_single_part";

  // ---- args ----
  std::string proxy = kProxy;
  std::uint64_t part_size = rtest::kDefaultPartSize;
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
            << "  part_size : " << rtest::HumanBytes(part_size) << "\n";

  // ---- GPU buffer ----
  void* dev_put = nullptr;
  if (cudaMalloc(&dev_put, part_size) != cudaSuccess) {
    std::cerr << "[FAIL] " << kTestName << ": cudaMalloc failed\n";
    return 1;
  }
  std::vector<std::byte> host(part_size);
  rtest::FillHostPattern(host);
  if (cudaMemcpy(dev_put, host.data(), part_size, cudaMemcpyHostToDevice) != cudaSuccess) {
    std::cerr << "[FAIL] " << kTestName << ": cudaMemcpy failed\n";
    cudaFree(dev_put);
    return 1;
  }

  // ---- init ----
  Client client(ClientOptions{.endpoint = proxy});
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    cudaFree(dev_put);
    return 1;
  }

  // ---- CreateMultipartUpload ----
  const std::string key = std::string("rtest-t13-gds-") + rtest::MakeTimestampSuffix();
  std::string upload_id, error;
  if (!client.CreateMultipartUpload(kBucket, key, PutDataPath::kGds, upload_id, error)) {
    std::cerr << "[FAIL] " << kTestName << ": CreateMultipartUpload: " << error << "\n";
    client.Shutdown();
    cudaFree(dev_put);
    return 1;
  }

  // ---- UploadPart + Complete ----
  bool test_passed = false;
  void* dev_get = nullptr;
  {
    std::string etag;
    if (!client.UploadPartGds(upload_id, 1, ConstBufferView{.data = dev_put, .size = part_size},
                              etag, error)) {
      std::cerr << "[FAIL] " << kTestName << ": UploadPartGds: " << error << "\n";
    } else {
      std::vector<Client::PartInfo> parts{{1, etag}};
      Client::CompletedMultipart done;
      if (!client.CompleteMultipartUpload(upload_id, parts, done)) {
        std::cerr << "[FAIL] " << kTestName << ": CompleteMultipartUpload: " << done.error << "\n";
      } else {
        std::cout << "  Complete: object_size=" << done.object_size << " etag=" << done.etag << "\n";
        test_passed = (done.object_size == part_size);
        if (!test_passed) {
          std::cerr << "[FAIL] " << kTestName << ": object_size mismatch: got=" << done.object_size
                    << " want=" << part_size << "\n";
        }
      }
    }
  }

  // ---- optional GET ----
  if (test_passed) {
    std::uint64_t obj_size = 0;
    std::string stat_err;
    if (!client.StatObject(kBucket, key, obj_size, stat_err) || obj_size != part_size) {
      std::cout << "  (optional GET skipped: StatObject failed)\n";
    } else if (cudaMalloc(&dev_get, obj_size) != cudaSuccess) {
      std::cout << "  (optional GET skipped: cudaMalloc failed)\n";
    } else {
      cudaMemset(dev_get, 0xAA, obj_size);
      GetPathResult get_res;
      if (client.GetObjectGds(kBucket, key, MutableBufferView{.data = dev_get, .size = obj_size},
                              get_res) && get_res.ok) {
        std::cout << "  GET: bytes_read=" << get_res.bytes_read << " crc32c=0x" << std::hex
                  << get_res.crc32c << std::dec << " hash=" << get_res.hash << "\n";
        if (!get_res.hash.empty() && get_res.bytes_read == obj_size) {
          std::cout << "  optional GET checks OK (hash non-empty)\n";
        }
        std::vector<std::byte> host_read(obj_size);
        cudaMemcpy(host_read.data(), dev_get, obj_size, cudaMemcpyDeviceToHost);
        rtest::VerifyHostBuffer(host_read.data(), obj_size, host, "single-part");
      } else {
        std::cout << "  (optional GET failed: " << get_res.error_message << ")\n";
      }
    }
  }

  // ---- cleanup ----
  if (dev_get) cudaFree(dev_get);
  cudaFree(dev_put);
  client.Shutdown();

  if (test_passed) {
    std::cout << "[PASS] " << kTestName << "\n";
    return 0;
  }
  return 1;
}
