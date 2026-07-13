#include "proxy/src/common/utils.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>

#include <openssl/evp.h>

namespace us3_turbo::proxy::utils {

namespace {

constexpr char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::string GenUuid() {
  static thread_local std::mt19937_64 rng{
      std::random_device{}() ^
      static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count())};
  std::uint64_t a = rng();
  std::uint64_t b = rng();
  unsigned char bytes[16];
  for (int i = 0; i < 8; ++i)
    bytes[i] = static_cast<unsigned char>(a >> (8 * (7 - i)));
  for (int i = 0; i < 8; ++i)
    bytes[i + 8] = static_cast<unsigned char>(b >> (8 * (7 - i)));
  bytes[6] = (bytes[6] & 0x0F) | 0x40;  // version 4
  bytes[8] = (bytes[8] & 0x3F) | 0x80;  // variant 10
  char buf[37];
  std::snprintf(buf, sizeof(buf),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                "%02x%02x%02x%02x%02x%02x",
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                bytes[12], bytes[13], bytes[14], bytes[15]);
  return std::string(buf);
}

std::string Sha1(std::string_view data) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  if (EVP_Digest(data.data(), data.size(), md, &md_len, EVP_sha1(), nullptr) !=
      1) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(md), md_len);
}

std::string Base64Encode(std::string_view data) {
  const std::size_t in_len = data.size();
  if (in_len == 0) return {};
  const std::size_t out_len = ((in_len + 2) / 3) * 4;
  std::string out;
  out.reserve(out_len);
  const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data());
  std::size_t i = 0;
  while (i + 3 <= in_len) {
    const std::uint32_t n = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
    out.push_back(kBase64Table[(n >> 18) & 0x3F]);
    out.push_back(kBase64Table[(n >> 12) & 0x3F]);
    out.push_back(kBase64Table[(n >> 6) & 0x3F]);
    out.push_back(kBase64Table[n & 0x3F]);
    i += 3;
  }
  const std::size_t rem = in_len - i;
  if (rem == 1) {
    const std::uint32_t n = p[i] << 16;
    out.push_back(kBase64Table[(n >> 18) & 0x3F]);
    out.push_back(kBase64Table[(n >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const std::uint32_t n = (p[i] << 16) | (p[i + 1] << 8);
    out.push_back(kBase64Table[(n >> 18) & 0x3F]);
    out.push_back(kBase64Table[(n >> 12) & 0x3F]);
    out.push_back(kBase64Table[(n >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string CombineETags(const std::vector<std::string>& etags) {
  if (etags.empty()) return {};
  if (etags.size() == 1) return etags[0];

  std::string concatenated;
  for (const auto& e : etags) concatenated += e;
  const std::string sha1 = Sha1(concatenated);

  std::string raw(4 + sha1.size(), '\0');
  const std::uint32_t cnt = static_cast<std::uint32_t>(etags.size());
  raw[0] = static_cast<char>(cnt & 0xff);
  raw[1] = static_cast<char>((cnt >> 8) & 0xff);
  raw[2] = static_cast<char>((cnt >> 16) & 0xff);
  raw[3] = static_cast<char>((cnt >> 24) & 0xff);
  raw.replace(4, sha1.size(), sha1);

  return Base64Encode(raw);
}

std::string Crc32cToETag(std::uint32_t crc32c) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08x", crc32c);
  return buf;
}

std::string CombineBlockCRC32s(const std::vector<std::uint32_t>& crcs) {
  if (crcs.empty()) return {};
  if (crcs.size() == 1) return Crc32cToETag(crcs[0]);

  /* 各 crc 按大端 4 字节拼接（与主机序无关），再 MD5 → 十六进制。 */
  std::string data;
  data.reserve(crcs.size() * 4);
  for (const std::uint32_t c : crcs) {
    data.push_back(static_cast<char>((c >> 24) & 0xff));
    data.push_back(static_cast<char>((c >> 16) & 0xff));
    data.push_back(static_cast<char>((c >> 8) & 0xff));
    data.push_back(static_cast<char>(c & 0xff));
  }

  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  if (EVP_Digest(data.data(), data.size(), md, &md_len, EVP_md5(), nullptr) !=
      1) {
    return {};
  }

  std::string hex;
  hex.reserve(md_len * 2);
  static constexpr char kHex[] = "0123456789abcdef";
  for (unsigned int i = 0; i < md_len; ++i) {
    hex.push_back(kHex[(md[i] >> 4) & 0xf]);
    hex.push_back(kHex[md[i] & 0xf]);
  }
  return hex;
}

}  // namespace us3_turbo::proxy::utils
