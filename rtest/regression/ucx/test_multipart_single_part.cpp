// test_multipart_single_part.cpp — T1.3 单 part = 整对象
//
// 验证: 单 part（part_number=1）的"不分段的分段上传" Complete 成功且
// object_size==part_size。
// 1 block/part: 单 part = 单块（block_size=part_size）→ GET 单块 →
// crc32c=该块 crc（非 0）、hash=Crc32cToETag(crc)。可选 GET 校验断言 hash 非空
// + bytes_read，不断言 crc32c 具体值。 失败条件: Complete 失败或
// object_size!=part_size。

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

namespace {
constexpr char kTestName[] = "ucx_multipart_single_part";
}

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = "192.168.1.198:9100";
  std::uint64_t part_size =
      rtest::kDefaultPartSize;  // 默认 4M（== proxy part 上限）

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
  const std::string key =
      std::string("rtest-t13-ucx-") + rtest::MakeTimestampSuffix();

  std::cout << "=== T1.3 UCX " << kTestName << " ===\n"
            << "  proxy     : " << proxy_addr << "\n"
            << "  part_size : " << rtest::HumanBytes(part_size) << "\n";

  // host buffer（PUT 与 GET 各一份）。
  std::vector<std::byte> put_buf(part_size);
  rtest::FillHostPattern(put_buf);

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

  // ---- CreateMultipartUpload ----
  std::string upload_id, error;
  if (!client.CreateMultipartUpload(bucket, key, PutDataPath::kUcx, upload_id,
                                    error)) {
    fail_reason = "CreateMultipartUpload failed: " + error;
    goto cleanup;
  }

  // ---- UploadPart (part 1) + Complete（同一作用域复用 etag）----
  {
    std::string etag;
    if (!client.UploadPartUcx(
            upload_id, 1,
            ConstBufferView{.data = put_buf.data(), .size = part_size}, etag,
            error)) {
      fail_reason = "UploadPartUcx 1 failed: " + error;
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
      fail_reason = "object_size mismatch: got " +
                    std::to_string(done.object_size) + " want " +
                    std::to_string(part_size);
      goto cleanup;
    }
    test_passed = true;
  }

  // ---- 可选 GET：验证 hash 非空 + bytes_read（不断言 crc32c!=0）----
  if (test_passed) {
    std::uint64_t obj_size = 0;
    std::string stat_err;
    if (!client.StatObject(bucket, key, obj_size, stat_err) ||
        obj_size != part_size) {
      std::cout << "  (optional GET skipped: StatObject failed or size "
                   "mismatch)\n";
    } else {
      std::vector<std::byte> get_buf(obj_size);
      std::memset(get_buf.data(), 0xAA, obj_size);
      GetPathResult get_res;
      if (client.GetObjectUcx(
              bucket, key,
              MutableBufferView{.data = get_buf.data(), .size = obj_size},
              get_res) &&
          get_res.ok) {
        std::cout << "  GET: bytes_read=" << get_res.bytes_read << " crc32c=0x"
                  << std::hex << get_res.crc32c << std::dec
                  << " hash=" << get_res.hash << "\n";
        // 1 block/part: 单 part = 单块 → crc32c = 该块 crc（非 0）。
        if (!get_res.hash.empty() && get_res.bytes_read == obj_size) {
          std::cout << "  optional GET checks OK (hash non-empty)\n";
        } else {
          std::cout << "  optional GET warning: hash empty or bytes_read!=size"
                    << "\n";
        }
        rtest::VerifyHostBuffer(get_buf.data(), obj_size, put_buf,
                                "single-part");
      } else {
        std::cout << "  (optional GET failed: " << get_res.error_message
                  << ")\n";
      }
    }
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
