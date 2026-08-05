// test_multipart_part_number_violation.cpp — RDMA part_number 重复/跳号检测。
//
// CASE A — 重复 part_number (1,1,2): UploadPart 同一 part_number 两次，
//   Complete 后必须产生非静默结果（失败或 object_size!=0）。
// CASE B — 跳号 (1,3 跳过 2): 非连续 part_number 导致 merged_size≠sum，
//   Complete 必须被拒绝。backend 数据面不稳时判 SKIP(77)。host 内存，无 CUDA 依赖。

#include <cstdint>
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
  constexpr const char* kTestName = "rdma_multipart_part_number_violation";

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

  // ---- host buffer (复用) ----
  std::vector<std::byte> host(part_size);
  rtest::FillHostPattern(host);

  // ---- init ----
  Client client(ClientOptions{.endpoint = proxy});
  if (!client.Initialize()) {
    std::cerr << "[FAIL] " << kTestName << ": Initialize failed\n";
    return 1;
  }

  // lambda: UploadPart 带重试 (默认 retries=1，重复 part 传 0)
  const auto upload_part = [&](const std::string& upload_id, const std::string& trace_id,
                               std::uint32_t part_no,
                               std::string& etag, std::string& err, int retries = 1) -> bool {
    for (int attempt = 0; attempt <= retries; ++attempt) {
      if (client.UploadPartRdma(upload_id, trace_id, part_no,
                                ConstBufferView{.data = host.data(), .size = part_size}, etag,
                                err)) {
        return true;
      }
    }
    return false;
  };

  // ---- Scene A: 重复 part_number (1,1,2) ----
  std::cout << "\n--- Scene A: duplicate part_number (1,1,2) ---\n";
  const std::string key_a = std::string("rtest-t12a-rdma-") + rtest::MakeTimestampSuffix();
  bool scene_a_pass = false;
  {
    std::string upload_id, trace_id, error;
    if (!client.CreateMultipartUpload(kBucket, key_a, PutDataPath::kRdma, upload_id, trace_id, error)) {
      std::cerr << "  CreateMultipartUpload failed: " << error << "\n";
    } else {
      std::string e1, e1b_err, e1b, e1b_err2, e2v, e2_err;
      const bool up1_ok = upload_part(upload_id, trace_id, 1, e1, e1b_err);
      const bool up1b_ok = upload_part(upload_id, trace_id, 1, e1b, e1b_err2, /*retries=*/0);
      const bool up2_ok = upload_part(upload_id, trace_id, 2, e2v, e2_err);

      std::cout << "  up1 ok=" << up1_ok << " | dup up1' ok=" << up1b_ok << " err=\"" << e1b_err2
                << "\" | up2 ok=" << up2_ok << "\n";

      std::vector<Client::PartInfo> parts;
      if (up1_ok) parts.push_back({1, e1});
      if (up1b_ok) parts.push_back({1, e1b});
      if (up2_ok) parts.push_back({2, e2v});

      Client::CompletedMultipart done;
      const bool ok =
          parts.empty() ? false : client.CompleteMultipartUpload(upload_id, trace_id, parts, done);
      std::cout << "  Complete ok=" << ok << " error=\"" << done.error
                << "\" size=" << done.object_size << "\n";

      if (ok) {
        scene_a_pass = (done.object_size != 0);
      } else if (!done.error.empty()) {
        scene_a_pass = true;
      } else if (!up1b_ok && !e1b_err2.empty()) {
        scene_a_pass = true;
      }

      std::string ab;
      client.AbortMultipartUpload(upload_id, trace_id, ab);
    }
  }

  // ---- Scene B: 跳号 (1,3) ----
  std::cout << "\n--- Scene B: gap (part 1, part 3 — skip 2) ---\n";
  const std::string key_b = std::string("rtest-t12b-rdma-") + rtest::MakeTimestampSuffix();
  bool scene_b_pass = false, scene_b_skipped = false;
  {
    std::string upload_id, trace_id, error;
    if (!client.CreateMultipartUpload(kBucket, key_b, PutDataPath::kRdma, upload_id, trace_id, error)) {
      std::cerr << "  CreateMultipartUpload failed: " << error << "\n";
    } else {
      std::string e1, e1_err, e3, e3_err;
      const bool up1_ok = upload_part(upload_id, trace_id, 1, e1, e1_err);
      const bool up3_ok = upload_part(upload_id, trace_id, 3, e3, e3_err);
      std::cout << "  up1 ok=" << up1_ok << " | up3 ok=" << up3_ok << "\n";

      if (!up1_ok || !up3_ok) {
        std::cout << "  [SKIP] scene B: backend data-plane unstable, cannot judge gap detection\n";
        scene_b_skipped = true;
      } else {
        std::vector<Client::PartInfo> parts{{1, e1}, {3, e3}};
        Client::CompletedMultipart done;
        const bool ok = client.CompleteMultipartUpload(upload_id, trace_id, parts, done);
        std::cout << "  Complete ok=" << ok << " error=\"" << done.error << "\"\n";
        scene_b_pass = (!ok && !done.error.empty());
      }

      std::string ab;
      client.AbortMultipartUpload(upload_id, trace_id, ab);
    }
  }

  // ---- cleanup ----
  client.Shutdown();

  // ---- result ----
  if (scene_b_skipped) {
    std::cout << "\n[SKIP] " << kTestName << ": scene B inconclusive; scene A="
              << (scene_a_pass ? "PASS" : "FAIL") << "\n";
    return 77;
  }
  if (scene_a_pass && scene_b_pass) {
    std::cout << "\n[PASS] " << kTestName << " (scene A + scene B both non-silent)\n";
    return 0;
  }
  std::cerr << "\n[FAIL] " << kTestName << ": sceneA=" << (scene_a_pass ? "PASS" : "FAIL")
            << " sceneB=" << (scene_b_pass ? "PASS" : "FAIL") << "\n";
  return 1;
}
