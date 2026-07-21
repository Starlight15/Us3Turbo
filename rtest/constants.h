// rtest/constants.h — 所有 example / bench / regression 共享的默认常量。
// 禁止各文件硬编码这些值；修改默认只需改这一处。
#pragma once

#include <cstdint>

namespace rtest {

// 网络
constexpr const char* kDefaultProxy = "192.168.1.198:9100";

// 存储命名
constexpr const char* kDefaultBucket = "test-bucket";
constexpr const char* kDefaultKeyPrefix = "bench";

// 对象大小（字节）
constexpr std::uint64_t kDefaultObjectSize = 4ULL * 1024 * 1024;       // 4 MiB
constexpr std::uint64_t kDefaultMultipartTotal = 64ULL * 1024 * 1024;  // 64 MiB
constexpr std::uint64_t kSinglePutMaxBytes = 16ULL * 1024 * 1024;      // 16 MiB
constexpr std::uint64_t kDefaultPartSize = 4ULL * 1024 * 1024;        // 4 MiB

// bench 默认参数
constexpr std::uint32_t kDefaultReplays = 5;
constexpr std::uint32_t kDefaultWarmup = 0;
constexpr std::uint32_t kDefaultConcurrency = 1;
constexpr std::uint32_t kDefaultNumParts = 2;

}  // namespace rtest
