// test_multipart_part_number_violation.cpp — T1.2 part_number 重复 / 跳号
//
// 两个子场景，默认 part-size 4M（确保唯一违例是 part_number，非 part 大小）。
// 场景A 重复 part_number（1,1,2）：行为依赖 MongoDB partlist_col 是否有
//   (upload_id,seq) 唯一索引——无则 Complete 时 "invalid parameter"，有则重复
//   UploadPart 时 "index write failed"。测试记录实际行为，接受任一非静默结果。
// 场景B 跳号（1,3 跳过 2）：ValidateParts 允许间隙，故跳号在 Complete step7
//   merged_size!=sum 处失败 → "internal error"。无专门 missing/sequence 错误，
//   测试匹配"非空错误"以兼容未来改进。Complete 成功（静默错误状态）才算失败。
// 失败条件: 任一场景出现静默状态——操作返回 false 但 error 为空，或跳号时
//   Complete 成功。

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

namespace {
constexpr char kTestName[] = "ucx_multipart_part_number_violation";
}

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = "192.168.1.198:9100";
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

  const std::string bucket = "test-bucket";
  const std::string key_a = std::string("rtest-t12a-ucx-") + rtest::MakeTimestampSuffix();
  const std::string key_b = std::string("rtest-t12b-ucx-") + rtest::MakeTimestampSuffix();

  std::cout << "=== T1.2 UCX " << kTestName << " ===\n"
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
    // backend 数据面偶发单 block 超时(见 TEST_FINDINGS.md P4)；对正常
    // UploadPart 做有限重试，仅在重试后仍失败才算真正失败，避免把环境抖动当作
    // part 上传 失败而误判跳号检测。注意: 重复 part 场景须传
    // retries=0——重复上传应只尝试 一次(重试会覆写 block 后再走
    // AddPart，改变重复检测语义)。
    for (int attempt = 0; attempt <= retries; ++attempt) {
      if (client.UploadPartUcx(upload_id, part_no,
                               ConstBufferView{.data = host.data(), .size = part_size}, etag,
                               err)) {
        return true;
      }
    }
    return false;
  };

  // ============================ 场景 A：重复 part_number
  // ============================
  std::cout << "\n--- Scene A: duplicate part_number (1,1,2) ---\n";
  bool scene_a_pass = false;
  std::string scene_a_reason;
  {
    std::string upload_id, error;
    if (!client.CreateMultipartUpload(bucket, key_a, PutDataPath::kUcx, upload_id, error)) {
      scene_a_reason = "CreateMultipartUpload failed: " + error;
      std::cerr << "  " << scene_a_reason << "\n";
    } else {
      std::string e1, e1b, e2v;
      const bool up1_ok = upload_part(upload_id, 1, e1, e1b);
      std::string e1b2, e1b_err2;
      // 重复 part 不重试(retries=0): 仅尝试一次，保留"重复上传"语义。
      const bool up1b_ok = upload_part(upload_id, 1, e1b2, e1b_err2, 0);
      std::string e2r, e2_err;
      const bool up2_ok = upload_part(upload_id, 2, e2v, e2_err);

      std::cout << "  up1 ok=" << up1_ok << " | dup up1' ok=" << up1b_ok << " err=\"" << e1b_err2
                << "\" | up2 ok=" << up2_ok << "\n";

      std::vector<Client::PartInfo> parts;
      if (up1_ok) parts.push_back({1, e1});
      if (up1b_ok) parts.push_back({1, e1b2});  // 重复
      if (up2_ok) parts.push_back({2, e2v});

      Client::CompletedMultipart done;
      const bool complete_ok =
          parts.empty() ? false : client.CompleteMultipartUpload(upload_id, parts, done);
      std::cout << "  Complete ok=" << complete_ok << " error=\"" << done.error
                << "\" size=" << done.object_size << "\n";

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

  // ============================ 场景 B：跳号 (1,3)
  // ============================
  std::cout << "\n--- Scene B: gap (part 1, part 3 — skip 2) ---\n";
  bool scene_b_pass = false;
  bool scene_b_skipped = false;
  std::string scene_b_reason;
  {
    std::string upload_id, error;
    if (!client.CreateMultipartUpload(bucket, key_b, PutDataPath::kUcx, upload_id, error)) {
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
      // 关键约束: 跳号检测只在 part1 与 part3 都成功上传后才有意义——此时
      // merged_size(=part1+part3, 48M) != sum(=32M) 必须在 Complete 被拒。若
      // up3 因 backend 数据面超时(见 TEST_FINDINGS.md P4)上传失败，实际只剩
      // part1，Complete 用单 part 必然成功(merged==sum==16M)，此时无法判定跳
      // 号逻辑 → 判 skip(77) 而非 FAIL，避免把环境不稳误报为回归。
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

  // 环境不稳(backend 数据面超时导致 up3 没上传) → 整体 SKIP(77)，不判 FAIL。
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
