# 阶段 2：实现 Proxy 内存态会话管理

## 目标

在 Proxy 端实现分段上传会话的内存态存储，支持会话创建、分段元数据追加、完成校验和清理。

---

## 约束

1. **纯内存**：使用 `std::unordered_map` 存储会话，不涉及数据库或文件持久化
2. **线程安全**：会话读写使用 `std::shared_mutex`（读多写少场景）
3. **TTL 清理**：会话超过 3 天自动过期（后台线程定期扫描）
4. **最小依赖**：仅依赖 C++17 标准库 + 阶段 1 的 proto 定义

---

## 数据结构设计（对齐 s3proxy）

### 文件：`proxy/src/multipart/upload_session.h`

```cpp
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "control_plane.pb.h"  // 阶段 1 生成的 proto

namespace us3_turbo::proxy {

// 单个 part 的元数据（对齐 s3proxy 的 Us3PartElement）
struct PartMetadata {
  uint32_t part_number;    // 1-based，对应 PartNum
  uint64_t part_size;      // 字节数，对应 PartSize
  std::string etag;        // 对应 Etag（US3 格式，SHA1 base64）
  std::string s3_etag;     // 对应 S3Etag（用于客户端校验）
  int64_t upload_time_ms;  // 上传完成时间戳（毫秒），对应 Modify
  
  // 注意：CRC32C 不存储在 part 级别，只在 block 级别用于传输校验
};

// 一个分段上传会话（对齐 s3proxy 的 Us3MinitIdxInfo）
struct UploadSession {
  // ===== 核心字段（对齐 s3proxy） =====
  std::string upload_id;      // 对应 UploadId
  std::string bucket;         // 对应 Bucket
  std::string key;            // 对应 Key
  std::string obj_id;         // 对应 ObjId（UUID，创建时生成）
  std::string file_id;        // 对应 Fileid（从 bucket 获取或生成）
  uint64_t set_id;            // 对应 Setid（存储 set ID，创建时选择）
  
  PutDataPath path;           // PATH_GDS 或 PATH_UCX（Us3Turbo 特有）
  
  uint64_t blk_size;          // 对应 BlkSize（默认 4MB）
  uint32_t blk_cnt;           // 对应 BlkCnt（已上传 block 总数）
  uint32_t max_blk;           // 对应 MaxBlk（最大 block 编号）
  
  int64_t created_at_ms;      // 创建时间戳（毫秒），对应 Expire 的基准
  int32_t status;             // 对应 Status（0=未完成, 1=已完成）
  std::string final_etag;     // 对应 Etag（完成后填充）
  
  // ===== 可选字段（后续对接 s3proxy 补充） =====
  // std::string content_type;
  // std::string acl;
  // std::map<std::string, std::string> tags;
  
  // ===== 运行时字段 =====
  std::vector<PartMetadata> parts;  // 已上传的 part 列表（对应 us3_partlist 表）
  mutable std::mutex parts_mu;      // 保护 parts 并发写
  
  // 辅助函数
  bool IsExpired(int64_t now_ms, int64_t ttl_ms) const {
    return (now_ms - created_at_ms) > ttl_ms;
  }
  
  uint64_t TotalSize() const {
    uint64_t sum = 0;
    for (const auto& p : parts) sum += p.part_size;
    return sum;
  }
};

}  // namespace us3_turbo::proxy
```

---

## 会话管理器实现

### 文件：`proxy/src/multipart/session_manager.h`

```cpp
#pragma once
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include "upload_session.h"

namespace us3_turbo::proxy {

class SessionManager {
 public:
  SessionManager();
  ~SessionManager();
  
  // 创建新会话，返回 upload_id（UUID）
  std::string CreateSession(const std::string& bucket,
                            const std::string& key,
                            PutDataPath path);
  
  // 获取会话（返回指针，调用者需检查空指针）
  UploadSession* GetSession(const std::string& upload_id);
  
  // 添加已上传的 part 元数据
  bool AddPart(const std::string& upload_id, const PartMetadata& part);
  
  // 完成会话：校验 part 列表，生成最终 object_id 和 etag
  // 返回 true 表示校验通过，out 参数被填充
  bool CompleteSession(const std::string& upload_id,
                       const std::vector<CompleteMultipartUploadRequest::PartInfo>& client_parts,
                       std::string& out_object_id,
                       std::string& out_etag,
                       uint64_t& out_size,
                       std::string& out_error);
  
  // 清理会话（立即删除）
  void CleanupSession(const std::string& upload_id);
  
  // 后台清理：删除超过 TTL 的会话
  void CleanupExpiredSessions(int64_t ttl_ms);
  
  // TODO: 后续接入 MongoDB 时新增接口
  // bool PersistSession(const std::string& upload_id);  // 持久化到 us3_minit
  // bool LoadSession(const std::string& upload_id);     // 从 us3_minit 加载
  
 private:
  std::unordered_map<std::string, UploadSession> sessions_;
  mutable std::shared_mutex sessions_mu_;  // 读写锁
  
  // 校验辅助函数
  bool ValidatePartList(const UploadSession& session, std::string& error);
  std::string ComputeFinalETag(const std::vector<PartMetadata>& parts);
  
  // TODO: 后续实现
  uint64_t SelectSetId();  // 选择存储 set（参考 s3proxy setcache.SelectSet）
};

}  // namespace us3_turbo::proxy
```

### 文件：`proxy/src/multipart/session_manager.cpp`

**伪代码实现关键逻辑：**

```cpp
#include "session_manager.h"
#include <algorithm>
#include <sstream>
#include <chrono>
// 假设有工具函数: utils::GenUuid(), utils::SHA1(), utils::Base64Encode()

std::string SessionManager::CreateSession(const std::string& bucket,
                                          const std::string& key,
                                          PutDataPath path) {
  std::string upload_id = utils::GenUuid();  // 生成 UUID
  int64_t now = std::chrono::system_clock::now().time_since_epoch().count() / 1000000;
  
  UploadSession session;
  session.upload_id = upload_id;
  session.bucket = bucket;
  session.key = key;
  session.path = path;
  session.created_at_ms = now;
  
  // ===== 对齐 s3proxy 的字段初始化 =====
  session.obj_id = utils::GenUuid();           // 生成对象 ID
  session.file_id = "";                        // TODO: 从 bucket_info 获取或生成
  session.set_id = SelectSetId();              // TODO: 调用 set 选择逻辑（参考 s3proxy setcache.SelectSet）
  session.blk_size = 4 * 1024 * 1024;          // 默认 4MB
  session.blk_cnt = 0;                         // 初始 0
  session.max_blk = 0;                         // 初始 0
  session.status = 0;                          // 0=未完成
  session.final_etag = "";                     // 完成后填充
  
  {
    std::unique_lock lock(sessions_mu_);
    sessions_[upload_id] = std::move(session);
  }
  
  return upload_id;
}

UploadSession* SessionManager::GetSession(const std::string& upload_id) {
  std::shared_lock lock(sessions_mu_);
  auto it = sessions_.find(upload_id);
  if (it == sessions_.end()) return nullptr;
  return &it->second;
}

bool SessionManager::AddPart(const std::string& upload_id, const PartMetadata& part) {
  UploadSession* session = GetSession(upload_id);
  if (!session) return false;
  
  std::lock_guard lock(session->parts_mu);
  
  // 去重：如果 part_number 已存在，替换为最新的（模拟 s3proxy 的 Modify timestamp 去重）
  auto it = std::find_if(session->parts.begin(), session->parts.end(),
                         [&](const auto& p) { return p.part_number == part.part_number; });
  if (it != session->parts.end()) {
    *it = part;  // 覆盖
  } else {
    session->parts.push_back(part);
  }
  
  // ===== 更新 s3proxy 对齐字段 =====
  // 计算本 part 包含的 block 数量（part_size / blk_size 向上取整）
  uint32_t part_block_cnt = (part.part_size + session->blk_size - 1) / session->blk_size;
  session->blk_cnt += part_block_cnt;
  session->max_blk = std::max(session->max_blk, session->blk_cnt - 1);  // max_blk 是最大索引
  
  return true;
}

bool SessionManager::CompleteSession(
    const std::string& upload_id,
    const std::vector<CompleteMultipartUploadRequest::PartInfo>& client_parts,
    std::string& out_object_id,
    std::string& out_etag,
    uint64_t& out_size,
    std::string& out_error) {
  
  UploadSession* session = GetSession(upload_id);
  if (!session) {
    out_error = "upload_id not found";
    return false;
  }
  
  std::lock_guard lock(session->parts_mu);
  
  // 1. 按 part_number 排序
  std::sort(session->parts.begin(), session->parts.end(),
            [](const auto& a, const auto& b) { return a.part_number < b.part_number; });
  
  // 2. 校验 part 顺序（必须连续：1, 2, 3, ...）
  if (!ValidatePartList(*session, out_error)) {
    return false;
  }
  
  // 3. 如果 client 提供了 part 列表，校验 etag 匹配
  if (!client_parts.empty()) {
    if (client_parts.size() != session->parts.size()) {
      out_error = "part count mismatch";
      return false;
    }
    for (size_t i = 0; i < client_parts.size(); ++i) {
      if (client_parts[i].part_number() != session->parts[i].part_number ||
          client_parts[i].etag() != session->parts[i].etag) {
        out_error = "part etag mismatch at part " + std::to_string(i+1);
        return false;
      }
    }
  }
  
  // 4. 生成最终 object_id 和 etag
  out_object_id = session->bucket + "/" + session->key;
  out_etag = ComputeFinalETag(session->parts);
  out_size = session->TotalSize();
  
  return true;
}

bool SessionManager::ValidatePartList(const UploadSession& session, std::string& error) {
  if (session.parts.empty()) {
    error = "no parts uploaded";
    return false;
  }
  
  // 检查 part_number 必须从 1 开始连续
  for (size_t i = 0; i < session.parts.size(); ++i) {
    if (session.parts[i].part_number != i + 1) {
      error = "part_number not consecutive, expected " + std::to_string(i+1) +
              " but got " + std::to_string(session.parts[i].part_number);
      return false;
    }
  }
  
  return true;
}

std::string SessionManager::ComputeFinalETag(const std::vector<PartMetadata>& parts) {
  // 参考 s3proxy 的 ETagByEtags 逻辑：
  // 如果只有 1 个 part，直接用该 part 的 etag
  // 如果多个 part，拼接所有 etag 后 SHA1，再 base64
  
  if (parts.size() == 1) {
    return parts[0].etag;
  }
  
  std::string concatenated;
  for (const auto& p : parts) {
    concatenated += p.etag;
  }
  
  std::string sha1 = utils::SHA1(concatenated);
  
  // 对齐 s3proxy：前缀 4 字节的 block count（little endian）
  std::vector<uint8_t> result(4);
  uint32_t blk_count = parts.size();
  result[0] = blk_count & 0xff;
  result[1] = (blk_count >> 8) & 0xff;
  result[2] = (blk_count >> 16) & 0xff;
  result[3] = (blk_count >> 24) & 0xff;
  
  result.insert(result.end(), sha1.begin(), sha1.end());
  
  return utils::Base64Encode(std::string(result.begin(), result.end()));
}

void SessionManager::CleanupSession(const std::string& upload_id) {
  std::unique_lock lock(sessions_mu_);
  sessions_.erase(upload_id);
}

void SessionManager::CleanupExpiredSessions(int64_t ttl_ms) {
  int64_t now = std::chrono::system_clock::now().time_since_epoch().count() / 1000000;
  
  std::unique_lock lock(sessions_mu_);
  for (auto it = sessions_.begin(); it != sessions_.end(); ) {
    if (it->second.IsExpired(now, ttl_ms)) {
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
}
```

---

## 工具函数（如不存在则需实现）

### 文件：`proxy/src/common/utils.h`

```cpp
#pragma once
#include <string>

namespace us3_turbo::proxy::utils {

// 生成 UUID v4 格式字符串
std::string GenUuid();

// 计算 SHA1 哈希（返回二进制字节串）
std::string SHA1(const std::string& data);

// Base64 编码
std::string Base64Encode(const std::string& data);

}  // namespace
```

可使用第三方库（如 `boost::uuid`, OpenSSL 的 `SHA1`, 或 C++20 的 `<random>` + 手写 base64）。

---

## 编译验证

```bash
# 1. 编译会话管理器
cd proxy
g++ -std=c++17 -I../proto -I../generated -c src/multipart/session_manager.cpp -o build/session_manager.o

# 2. 链接测试（需要 proto 库）
g++ -std=c++17 build/session_manager.o ../generated/control_plane.pb.o -lprotobuf -lpthread -o build/test_session

# 3. 单元测试伪代码（后续可用 gtest）
# - 创建 10 个会话，验证 upload_id 唯一
# - 添加 3 个 part，验证 parts.size() == 3
# - CompleteSession 校验 part_number 连续性
# - CleanupExpiredSessions 验证过期会话被删除
```

---

## 验收标准

- [ ] `SessionManager::CreateSession` 返回有效 UUID
- [ ] `GetSession` 能正确返回已创建的会话指针
- [ ] `AddPart` 支持同一 part_number 的覆盖（去重）
- [ ] `CompleteSession` 能检测 part_number 不连续的错误
- [ ] `ComputeFinalETag` 对单 part 和多 part 都有合理输出
- [ ] `CleanupExpiredSessions` 能删除超过 TTL 的会话

---

## 后续阶段依赖

- **阶段 3**：Proxy RPC 服务实现（调用本阶段的 `SessionManager`）
- **阶段 4**：Proxy 切分逻辑（依赖会话中的 part 元数据）
