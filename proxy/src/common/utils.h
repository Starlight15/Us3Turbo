#pragma once

// utils.h — proxy 通用工具：UUID / SHA1 / Base64。
//
// 分段上传会话需生成 upload_id（UUID）与会话/对象 ETag（SHA1 拼接后 base64）。
// 实现走 OpenSSL（libcrypto）：SHA1 用 EVP；base64 手写以控制无换行/填充策略
// 失败时返回空串，调用方按空判错。

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace us3_turbo::proxy::utils {

/* 生成 UUID v4 字符串（xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx）。 */
[[nodiscard]] std::string GenUuid();

/* SHA1 摘要，返回 20 字节二进制串；失败返回空串。 */
[[nodiscard]] std::string Sha1(std::string_view data);

/* Base64 编码，标准字母表无换行；失败返回空串。 */
[[nodiscard]] std::string Base64Encode(std::string_view data);

/* 当前时间戳（毫秒，system_clock）。 */
[[nodiscard]] std::int64_t NowMs();

/* 汇总多个 etag：单元素直接返回；
 * 多元素 → 4 字节 LE count + SHA1 拼接 + base64。
 * SinglePut 与 Multipart 共用此纯算法。 */
[[nodiscard]] std::string CombineETags(const std::vector<std::string>& etags);

/* 单个 CRC32C → 8 位十六进制 ETag，单 block 与单步 PUT 复用。
 * backend 不返回 etag，以 crc32c 十六进制占位。
 * 非 S3 标准 etag，仅作标识。 */
[[nodiscard]] std::string Crc32cToETag(std::uint32_t crc32c);

/* 组合多个 block CRC32C 生成 part 级 ETag：
 * 空→空串；单→Crc32cToETag；多→MD5(大端拼接)十六进制。
 * 大端字节序保证跨平台一致；MD5 走 EVP 与 Sha1 同栈。 */
[[nodiscard]] std::string CombineBlockCRC32s(const std::vector<std::uint32_t>& crcs);

/* 计算从 start 到现在的耗时（毫秒）。
 * steady_clock 单调，不受系统时钟跳变影响。
 * inline 定义在头文件，避免多翻译单元符号重复。 */
[[nodiscard]] inline std::chrono::milliseconds ElapsedMs(
    std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start);
}

}  // namespace us3_turbo::proxy::utils
