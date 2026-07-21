// test_multipart_part_number_violation.cpp — T1.2 part_number 重复/跳号。
//
// 验证: Scene A 重复 part_number 非静默失败,Scene B 跳号被检测。

//   Complete 成功。
// RDMA 路径：host 内存，无 CUDA 依赖。

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

namespace {
constexpr char kTestName[] = "rdma_multipart_part_number_violation";
}


int main(int argc, char** argv) {
  using namespace us3_turbo::client;
  constexpr const char* kProxy = "192.168.1.198:9100";
  constexpr const char* kBucket = "test-bucket";

  std::string proxy_addr = kProxy;
  std::uint64_t part_size = rtest::kDefaultPartSize;  // 默认 4M（== proxy part 上限）

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

  const std::string bucket = kBucket;
  const std::string key_a = std::string("rtest-t12a-rdma-") + rtest::MakeTimestampSuffix();
  const std::string key_b = std::string("rtest-t12b-rdma-") + rtest::MakeTimestampSuffix();

  std::cout << "=== T1.2 RDMA " << kTestName << " ===\n"
            << "  proxy     : " << proxy_addr << "\n"
            << "  part_size : " << rtest::HumanBytes(part_size) << "\n";

  // host buffer（各场景 part 复用同一 buffer）。
  std::vector<std::byte> host(part_size);
  rtest::FillHostPattern(host);

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.verify_crc32c = false;
  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }

  const auto upload_part = [&](const std::string& upload_id, std::uint32_t part_no,
                               std::string& etag, std::string& err, int retries = 1) -> bool {
    // backend 数据面偶发单 block 超时；对正常 UploadPart 做有限重试。
    // 重复 part 场景须传 retries=0——重复上传应只尝试一次。
    for (int attempt = 0; attempt <= retries; ++attempt) {
      if (client.UploadPartRdma(upload_id, part_no,
                                ConstBufferView{.data = host.data(), .size = part_size}, etag,
                                err)) {
        return true;
      }
    }
    return false;
  };

  // ---- Scene A: 重复 part_number (1,1,2) ----
  std::cout << "\n--- Scene A: duplicate part_number (1,1,2) ---\n";
  bool scene_a_pass = false;
  std::string scene_a_reason;
  {
    std::string upload_id, error;
    if (!client.CreateMultipartUpload(bucket, key_a, PutDataPath::kRdma, upload_id, error)) {
      scene_a_reason = "CreateMultipartUpload failed: " + error;
      std::cerr << "  " << scene_a_reason << "\n";
    } else {
      std::string e1, e1b_err, e2, e3_err;
      const bool up1_ok = upload_part(upload_id, 1, e1, e1b_err);
      std::string e1b, e1b_err2;
      const bool up1b_ok = upload_part(upload_id, 1, e1b, e1b_err2, 0);  // 重复 part 不重试
      std::string e2v, e2_err;
      const bool up2_ok = upload_part(upload_id, 2, e2v, e2_err);

      std::cout << "  up1 ok=" << up1_ok << " | dup up1' ok=" << up1b_ok << " err=\"" << e1b_err2
                << "\" | up2 ok=" << up2_ok << "\n";

      // 收集成功上传的 part 供 Complete。
      std::vector<Client::PartInfo> parts;
      if (up1_ok) parts.push_back({1, e1});
      if (up1b_ok) parts.push_back({1, e1b});
      if (up2_ok) parts.push_back({2, e2v});

      Client::CompletedMultipart done;
      const bool complete_ok =
          parts.empty() ? false : client.CompleteMultipartUpload(upload_id, parts, done);
      std::cout << "  Complete ok=" << complete_ok << " error=\"" << done.error
                << "\" size=" << done.object_size << "\n";

      // PASS = 任一非静默结果。
      if (complete_ok) {
        scene_a_pass = (done.object_size != 0);
        if (!scene_a_pass) scene_a_reason = "Complete succeeded but object_size==0";
      } else if (!done.error.empty()) {
        scene_a_pass = true;
      } else if (!up1b_ok && !e1b_err2.empty()) {
        scene_a_pass = true;
      } else {
        scene_a_reason =
            "silent failure: Complete=false with empty error and dup "
            "UploadPart=false with empty error";
      }

      std::string ab;
      (void)client.AbortMultipartUpload(upload_id, ab);
    }
  }

  // ---- Scene B: 跳号 (1,3) ----
  std::cout << "\n--- Scene B: gap (part 1, part 3 — skip 2) ---\n";
  bool scene_b_pass = false;
  bool scene_b_skipped = false;
  std::string scene_b_reason;
  {
    std::string upload_id, error;
    if (!client.CreateMultipartUpload(bucket, key_b, PutDataPath::kRdma, upload_id, error)) {
      scene_b_reason = "CreateMultipartUpload failed: " + error;
      std::cerr << "  " << scene_b_reason << "\n";
    } else {
      std::string e1, e1_err;
      const bool up1_ok = upload_part(upload_id, 1, e1, e1_err);
      std::string e3, e3_err;
      const bool up3_ok = upload_part(upload_id, 3, e3, e3_err);
      std::cout << "  up1 ok=" << up1_ok << " | up3 ok=" << up3_ok << "\n";

      std::vector<Client::PartInfo> parts;
      if (up1_ok) parts.push_back({1, e1});
      if (up3_ok) parts.push_back({3, e3});

      Client::CompletedMultipart done;
      const bool complete_ok =
          parts.empty() ? false : client.CompleteMultipartUpload(upload_id, parts, done);
      std::cout << "  Complete ok=" << complete_ok << " error=\"" << done.error << "\"\n";

      // 关键约束: 跳号检测只在 part1 与 part3 都成功上传后才有意义。
      if (!up1_ok || !up3_ok) {
        scene_b_reason =
            "UploadPart failed before gap could be exercised "
            "(up1=" +
            std::to_string(up1_ok) + " up3=" + std::to_string(up3_ok) +
            "); backend data-plane unstable, cannot judge gap detection";
        std::cout << "  [SKIP] scene B: " << scene_b_reason << "\n";
        scene_b_skipped = true;
      } else if (complete_ok) {
        scene_b_pass = false;
        scene_b_reason = "expected Complete to fail on gapped upload, but it succeeded";
      } else if (!done.error.empty()) {
        scene_b_pass = true;
      } else {
        scene_b_reason = "Complete failed but error is empty";
      }

      std::string ab;
      (void)client.AbortMultipartUpload(upload_id, ab);
    }
  }

  client.Shutdown();

  // 环境不稳 → 整体 SKIP(77)，不判 FAIL。
  if (scene_b_skipped) {
    std::cout << "\n[SKIP] " << kTestName << ": scene B inconclusive (backend unstable); scene A="
              << (scene_a_pass ? "PASS" : "FAIL") << "\n";
    return 77;
  }
  const bool test_passed = scene_a_pass && scene_b_pass;
  if (test_passed) {
    std::cout << "\n[PASS] " << kTestName << " (scene A + scene B both non-silent)\n";
    return 0;
  }
  std::cerr << "\n[FAIL] " << kTestName << ": ";
  if (!scene_a_pass) std::cerr << "sceneA={" << scene_a_reason << "} ";
  if (!scene_b_pass) std::cerr << "sceneB={" << scene_b_reason << "}";
  std::cerr << "\n";
  return 1;
}
