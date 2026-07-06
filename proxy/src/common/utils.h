#pragma once

// utils.h — proxy 通用工具：UUID / SHA1 / Base64。
//
// 分段上传会话需生成 upload_id（UUID）与会话/对象 ETag（SHA1 拼接后 base64）。
// 实现走 OpenSSL（libcrypto）：SHA1 用 EVP；base64 手写以控制无换行/填充策略
// 失败时返回空串，调用方按空判错。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace us3_turbo::proxy::utils {

/** @brief 生成 UUID v4 格式字符串（xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx）。 */
[[nodiscard]] std::string GenUuid();

/** @brief SHA1(data) 返回 20 字节二进制串（失败返回空串）。 */
[[nodiscard]] std::string Sha1(std::string_view data);

/** @brief Base64 编码（标准字母表，无换行；失败返回空串）。 */
[[nodiscard]] std::string Base64Encode(std::string_view data);

/** @brief 当前时间戳（毫秒，system_clock）。 */
[[nodiscard]] std::int64_t NowMs();

/**
 * @brief 汇总多个 etag：单元素直接返回；多元素 → 4 字节 LE count + SHA1 拼接 + base64。
 *
 * 与链路无关的纯算法，SessionManager（part→object）与 MultipartPutHandler
 * （block→part）汇总共用，避免两处逐字节重复。
 */
[[nodiscard]] std::string CombineETags(const std::vector<std::string>& etags);

}  // namespace us3_turbo::proxy::utils
