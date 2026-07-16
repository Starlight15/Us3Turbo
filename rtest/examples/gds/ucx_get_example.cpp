// ucx_get_example.cpp — UCX 单步 + 分段 PUT 后 GET 读回端到端验证。
//
// 流程：
//   1. UCX 单步 PUT 一个小对象（≤16MiB，host 内存）
//   2. UCX 分段 PUT 一个大对象（多 part，host 内存）
//   3. 对每个对象：StatObject 查布局 → 分配 host buffer → GetObjectUcx 读回
//   4. 逐字节比对
//
// 用法：
//   us3_turbo_ucx_get_example \
//     --proxy 192.168.1.198:9100 \
//     --single-size 4M \
//     --part-size 16M --num-parts 2 \
//     [--verify-crc32c]

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "client/src/common/request.h"
#include "us3_turbo/client/client.h"

namespace {

using clk = std::chrono::steady_clock;
using ms_double = std::chrono::duration<double, std::milli>;

bool ParseSize(std::string_view s, std::uint64_t& out) {
  if (s.empty()) return false;
  std::uint64_t num = 0;
  std::size_t i = 0;
  for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++i) {
    num = num * 10 + static_cast<std::uint64_t>(s[i] - '0');
  }
  if (i == 0) return false;
  std::uint64_t mul = 1;
  if (i < s.size()) {
    if (i + 1 != s.size()) return false;
    switch (std::tolower(static_cast<unsigned char>(s[i]))) {
      case 'b':
        mul = 1ULL;
        break;
      case 'k':
        mul = 1024ULL;
        break;
      case 'm':
        mul = 1024ULL * 1024;
        break;
      case 'g':
        mul = 1024ULL * 1024 * 1024;
        break;
      default:
        return false;
    }
  }
  out = num * mul;
  return true;
}

std::string HumanBytes(std::uint64_t b) {
  constexpr double K = 1024.0;
  char buf[64];
  if (b >= static_cast<std::uint64_t>(K * K * K))
    std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(b) / (K * K * K));
  else if (b >= static_cast<std::uint64_t>(K * K))
    std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(b) / (K * K));
  else if (b >= static_cast<std::uint64_t>(K))
    std::snprintf(buf, sizeof(buf), "%.2f KiB", static_cast<double>(b) / K);
  else
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(b));
  return buf;
}

// 生成确定性 pattern 到 host buffer。
void FillPattern(std::vector<std::byte>& buf, std::uint64_t offset_base = 0) {
  for (std::size_t i = 0; i < buf.size(); ++i) {
    buf[i] = static_cast<std::byte>((i + offset_base) % 251U);
  }
}

// 逐字节比对 host buffer。
bool VerifyGet(const void* host_buf, std::size_t size,
               const std::vector<std::byte>& expected, const std::string& tag) {
  const auto* read = static_cast<const std::byte*>(host_buf);
  if (size != expected.size()) {
    std::cerr << "[" << tag << "] size mismatch: got " << size << " want "
              << expected.size() << "\n";
    return false;
  }
  std::size_t mismatches = 0;
  std::size_t first_mismatch = 0;
  for (std::size_t i = 0; i < size; ++i) {
    if (read[i] != expected[i]) {
      if (mismatches == 0) first_mismatch = i;
      ++mismatches;
    }
  }
  if (mismatches > 0) {
    std::cerr << "[" << tag << "] DATA MISMATCH: " << mismatches
              << " bytes differ, first at offset " << first_mismatch << " (got 0x"
              << std::hex << static_cast<unsigned>(read[first_mismatch]) << " want 0x"
              << static_cast<unsigned>(expected[first_mismatch]) << std::dec << ")\n";
    return false;
  }
  std::cout << "[" << tag << "] data VERIFIED OK (" << HumanBytes(size) << ")\n";
  return true;
}

// ========== 单步 PUT + GET 验证 ==========

bool TestSinglePutGet(us3_turbo::client::Client& client, const std::string& bucket,
                      const std::string& key, std::uint64_t single_size,
                      bool verify_crc32c) {
  using namespace us3_turbo::client;

  std::cout << "\n========== Single UCX PUT + GET ==========\n"
            << "  key       : " << key << "\n"
            << "  size      : " << HumanBytes(single_size) << "\n";

  // 分配 host buffer，PUT 和 GET 共用
  std::vector<std::byte> put_data(single_size);
  FillPattern(put_data);

  // ---- PUT ----
  ClientProxyPutRequest put_req;
  put_req.bucket = bucket;
  put_req.key = key;
  put_req.object_size = single_size;
  put_req.path = PutDataPath::kUcx;

  ClientProxyPutResponse put_resp;
  const auto t0 = clk::now();
  bool put_ok = client.PutObject(
      put_req, ConstBufferView{.data = put_data.data(), .size = single_size}, put_resp);
  const auto t1 = clk::now();

  if (!put_ok) {
    std::cerr << "Single UCX PUT FAILED\n";
    return false;
  }
  const auto& pr = put_resp.ucx_result.value();
  const double put_ms = ms_double(t1 - t0).count();
  const double put_mbs = (put_ms > 0.0) ? static_cast<double>(single_size) /
                                              (put_ms / 1000.0) / (1024.0 * 1024.0)
                                        : 0.0;
  std::cout << "  PUT OK: bytes=" << pr.bytes_written << " etag=" << pr.etag
            << " crc32c=0x" << std::hex << pr.crc32c << std::dec << " wall=" << put_ms
            << "ms"
            << " throughput=" << put_mbs << " MiB/s\n";

  // ---- StatObject ----
  std::uint64_t object_size = 0;
  std::string stat_error;
  if (!client.StatObject(bucket, key, object_size, stat_error)) {
    std::cerr << "StatObject FAILED: " << stat_error << "\n";
    return false;
  }
  std::cout << "  StatObject OK: object_size=" << HumanBytes(object_size) << "\n";
  if (object_size != single_size) {
    std::cerr << "  StatObject size mismatch: got " << object_size << " want "
              << single_size << "\n";
    return false;
  }

  // ---- GET：清零同一 buffer，然后读回 ----
  std::vector<std::byte> get_data(single_size);
  std::memset(get_data.data(), 0xBB, single_size);

  GetPathResult get_result;
  const auto t2 = clk::now();
  bool get_ok = client.GetObjectUcx(
      bucket, key, MutableBufferView{.data = get_data.data(), .size = object_size},
      get_result);
  const auto t3 = clk::now();

  if (!get_ok || !get_result.ok) {
    std::cerr << "GET FAILED: " << get_result.error_message << "\n";
    return false;
  }
  const double get_ms = ms_double(t3 - t2).count();
  const double get_mbs = (get_ms > 0.0) ? static_cast<double>(object_size) /
                                              (get_ms / 1000.0) / (1024.0 * 1024.0)
                                        : 0.0;
  std::cout << "  GET OK: bytes_read=" << get_result.bytes_read << " crc32c=0x"
            << std::hex << get_result.crc32c << std::dec << " hash=" << get_result.hash
            << " wall=" << get_ms << "ms"
            << " throughput=" << get_mbs << " MiB/s\n";

  if (get_result.bytes_read != object_size) {
    std::cerr << "  GET bytes_read mismatch: got " << get_result.bytes_read << " want "
              << object_size << "\n";
    return false;
  }

  // ---- 逐字节比对 ----
  bool verified = VerifyGet(get_data.data(), object_size, put_data, "single-ucx");

  if (verify_crc32c && get_result.crc32c != 0) {
    std::cout << "  remote crc32c=0x" << std::hex << get_result.crc32c << std::dec
              << "\n";
  }

  return verified;
}

// ========== 分段 PUT + GET 验证 ==========

bool TestMultipartPutGet(us3_turbo::client::Client& client, const std::string& bucket,
                         const std::string& key, std::uint64_t part_size,
                         std::uint32_t num_parts, bool verify_crc32c) {
  using namespace us3_turbo::client;

  const std::uint64_t total = part_size * num_parts;
  std::cout << "\n========== Multipart UCX PUT + GET ==========\n"
            << "  key       : " << key << "\n"
            << "  part_size : " << HumanBytes(part_size) << "\n"
            << "  num_parts : " << num_parts << "\n"
            << "  total     : " << HumanBytes(total) << "\n";

  // ---- 构造完整 host 数据（各 part 用不同 pattern）----
  std::vector<std::byte> host_full(total);
  for (std::uint32_t p = 0; p < num_parts; ++p) {
    std::vector<std::byte> part_buf(part_size);
    FillPattern(part_buf, static_cast<std::uint64_t>(p) * part_size);
    std::memcpy(host_full.data() + p * part_size, part_buf.data(), part_size);
  }

  // ---- PUT buffer 和 GET buffer ----
  std::vector<std::byte> put_buf(part_size);
  std::vector<std::byte> get_buf(total);

  // ---- CreateMultipartUpload ----
  std::string upload_id, error;
  if (!client.CreateMultipartUpload(bucket, key, PutDataPath::kUcx, upload_id, error)) {
    std::cerr << "CreateMultipartUpload failed: " << error << "\n";
    return false;
  }
  std::cout << "  CreateMultipartUpload: upload_id=" << upload_id << "\n";

  // ---- Upload each part ----
  std::vector<Client::PartInfo> parts;
  parts.reserve(num_parts);

  const auto t0 = clk::now();
  for (std::uint32_t i = 1; i <= num_parts; ++i) {
    // 准备该 part 数据到 host buffer
    FillPattern(put_buf, static_cast<std::uint64_t>(i - 1) * part_size);

    std::string etag;
    if (!client.UploadPartUcx(upload_id, i,
                              ConstBufferView{.data = put_buf.data(), .size = part_size},
                              etag, error)) {
      std::cerr << "UploadPartUcx " << i << " failed: " << error << "\n";
      return false;
    }
    std::cout << "  UploadPartUcx part " << i << " etag=" << etag << "\n";
    parts.push_back({i, etag});
  }

  // ---- CompleteMultipartUpload ----
  Client::CompletedMultipart done;
  if (!client.CompleteMultipartUpload(upload_id, parts, done)) {
    std::cerr << "CompleteMultipartUpload failed: " << done.error << "\n";
    return false;
  }
  const auto t1 = clk::now();
  const double put_ms = ms_double(t1 - t0).count();
  const double put_mbs =
      (put_ms > 0.0) ? static_cast<double>(total) / (put_ms / 1000.0) / (1024.0 * 1024.0)
                     : 0.0;

  std::cout << "  CompleteMultipartUpload: object_id=" << done.object_id
            << " size=" << done.object_size << " etag=" << done.etag << " wall=" << put_ms
            << "ms"
            << " throughput=" << put_mbs << " MiB/s\n";

  if (done.object_size != total) {
    std::cerr << "  Complete size mismatch: got " << done.object_size << " want " << total
              << "\n";
    return false;
  }

  // ---- StatObject ----
  std::uint64_t object_size = 0;
  std::string stat_error;
  if (!client.StatObject(bucket, key, object_size, stat_error)) {
    std::cerr << "StatObject FAILED: " << stat_error << "\n";
    return false;
  }
  std::cout << "  StatObject OK: object_size=" << HumanBytes(object_size) << "\n";
  if (object_size != total) {
    std::cerr << "  StatObject size mismatch: got " << object_size << " want " << total
              << "\n";
    return false;
  }

  // ---- GET ----
  std::memset(get_buf.data(), 0xCC, object_size);

  GetPathResult get_result;
  const auto t2 = clk::now();
  bool get_ok = client.GetObjectUcx(
      bucket, key, MutableBufferView{.data = get_buf.data(), .size = object_size},
      get_result);
  const auto t3 = clk::now();

  if (!get_ok || !get_result.ok) {
    std::cerr << "GET FAILED: " << get_result.error_message << "\n";
    return false;
  }
  const double get_ms = ms_double(t3 - t2).count();
  const double get_mbs = (get_ms > 0.0) ? static_cast<double>(object_size) /
                                              (get_ms / 1000.0) / (1024.0 * 1024.0)
                                        : 0.0;
  std::cout << "  GET OK: bytes_read=" << get_result.bytes_read << " crc32c=0x"
            << std::hex << get_result.crc32c << std::dec << " hash=" << get_result.hash
            << " wall=" << get_ms << "ms"
            << " throughput=" << get_mbs << " MiB/s\n";

  if (get_result.bytes_read != object_size) {
    std::cerr << "  GET bytes_read mismatch: got " << get_result.bytes_read << " want "
              << object_size << "\n";
    return false;
  }

  // ---- 逐字节比对 ----
  bool verified = VerifyGet(get_buf.data(), object_size, host_full, "multipart-ucx");

  if (verify_crc32c && get_result.crc32c != 0) {
    std::cout << "  remote crc32c=0x" << std::hex << get_result.crc32c << std::dec
              << "\n";
  }

  return verified;
}

// ========== 不存在的 key → 正确返回错误 ==========

bool TestStatNonExistent(us3_turbo::client::Client& client, const std::string& bucket) {
  using namespace us3_turbo::client;

  std::cout << "\n========== StatObject on non-existent key ==========\n";

  std::uint64_t object_size = 0;
  std::string error;
  if (client.StatObject(bucket, "non_existent_key_12345", object_size, error)) {
    std::cerr << "  UNEXPECTED: StatObject succeeded for non-existent key\n";
    return false;
  }
  std::cout << "  OK: StatObject correctly failed: " << error << "\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = "192.168.1.198:9100";
  std::uint64_t single_size = 4ULL * 1024 * 1024;  // 默认 4MiB
  std::uint64_t part_size =
      16ULL * 1024 * 1024;  // 默认 16MiB per part（须与 proxy multipart_part_size 一致）
  std::uint32_t num_parts = 2;  // 默认 2 parts = 32MiB
  bool verify = false;

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
    } else if (arg == "--single-size") {
      std::string v;
      if (!need(v) || !ParseSize(v, single_size)) {
        std::cerr << "bad --single-size\n";
        return 2;
      }
    } else if (arg == "--part-size") {
      std::string v;
      if (!need(v) || !ParseSize(v, part_size)) {
        std::cerr << "bad --part-size\n";
        return 2;
      }
    } else if (arg == "--num-parts") {
      std::string v;
      if (!need(v)) return 2;
      num_parts = static_cast<std::uint32_t>(std::strtoull(v.c_str(), nullptr, 10));
      if (num_parts == 0) {
        std::cerr << "bad --num-parts\n";
        return 2;
      }
    } else if (arg == "--verify-crc32c") {
      verify = true;
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  const std::string bucket = "test-bucket";

  std::cout << "=== UCX PUT+GET E2E Verification ===\n"
            << "  proxy       : " << proxy_addr << "\n"
            << "  single size : " << HumanBytes(single_size) << "\n"
            << "  multipart   : " << HumanBytes(part_size) << " x " << num_parts << " = "
            << HumanBytes(part_size * num_parts) << "\n"
            << "  verify-crc  : " << (verify ? "on" : "off") << "\n"
            << std::endl;

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.verify_crc32c = verify;

  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "Initialize failed\n";
    return 1;
  }

  int failures = 0;

  // 使用时间戳后缀避免与上次运行残留数据冲突
  const auto ts = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());

  // 1. 不存在的 key → 正确报错
  if (!TestStatNonExistent(client, bucket)) ++failures;

  // 2. 单步 PUT + GET
  if (!TestSinglePutGet(client, bucket, "ucx-sgl-" + ts, single_size, verify)) {
    ++failures;
  }

  // 3. 分段 PUT + GET
  if (!TestMultipartPutGet(client, bucket, "ucx-mp-" + ts, part_size, num_parts,
                           verify)) {
    ++failures;
  }

  client.Shutdown();

  std::cout << "\n=== Result: " << (failures == 0 ? "ALL PASSED" : "FAILED") << " ("
            << failures << " failure(s)) ===\n";
  return failures == 0 ? 0 : 1;
}
