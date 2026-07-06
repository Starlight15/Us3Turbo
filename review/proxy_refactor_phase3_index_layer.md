# Proxy 分层重构 — 阶段 3：抽出索引层（IUploadIndex + InMemoryUploadIndex，mock）

**前置**：阶段 2 完成（服务层就位，接口层薄委托）。

**目标**：把 SessionManager 拆成两半——**纯 CRUD** 下沉到 `InMemoryUploadIndex`
（实现 `IUploadIndex` 接口）；**业务规则**（part 校验 / final etag / client etag 比对）
上移到 MultipartService。删除 `multipart/session_manager.*` 与 `multipart/upload_session.h`。
**为什么先 mock**：接口隔离后，后续 MongoDB 实现同一接口即可替换，服务层零改动。

**范围**：2 个新文件（index 层）+ MultipartService 补业务规则 + 删旧文件。

---

## 约束

- **索引层是纯被动存储**：`IUploadIndex` 只做元数据 CRUD，**不含**任何校验、etag 计算、
  client 比对——这些全部在 MultipartService。这样内存 mock 与 Mongo 实现可无差别替换。
- 记录结构体去掉业务方法（`TotalSize`/`IsExpired` 不再挂记录上）。
- 并发模型沿用旧 SessionManager（shared_mutex + 单 session parts_mu）。

---

## 改动 1：index/upload_index.h（接口 + 记录）

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "control_plane.pb.h"

namespace us3_turbo::proxy {

// part 元数据（对齐 s3proxy Us3PartElement，去掉业务方法）。
struct PartRecord {
  std::uint32_t part_number{0};
  std::uint64_t part_size{0};
  std::string   etag;
  std::int64_t  upload_time_ms{0};
};

// 会话元数据（对齐 s3proxy Us3MinitIdxInfo，去掉 TotalSize/IsExpired）。
struct UploadRecord {
  std::string  upload_id;
  std::string  bucket;
  std::string  key;
  PutDataPath  path{PATH_NONE};
  std::int64_t created_at_ms{0};
};

// 纯被动元数据存储接口。内存 mock 与后续 MongoDB 实现同一接口，可无差别替换。
// 不含任何校验 / etag 计算 / client 比对（全在服务层）。
class IUploadIndex {
 public:
  virtual ~IUploadIndex() = default;

  [[nodiscard]] virtual std::string Create(
      const std::string& bucket, const std::string& key, PutDataPath path) = 0;
  [[nodiscard]] virtual bool Get(
      const std::string& upload_id, UploadRecord& out) = 0;
  [[nodiscard]] virtual bool AddPart(
      const std::string& upload_id, const PartRecord& part) = 0;
  [[nodiscard]] virtual bool ListParts(
      const std::string& upload_id, std::vector<PartRecord>& out) = 0;
  virtual void Remove(const std::string& upload_id) = 0;
  virtual void RemoveExpired(std::int64_t ttl_ms) = 0;
};

}  // namespace us3_turbo::proxy
```

## 改动 2：index/in_memory_upload_index.{h,cpp}

内部结构（照搬旧 UploadSession 的并发模型，但只存数据）：
```cpp
// in_memory_upload_index.h
class InMemoryUploadIndex final : public IUploadIndex {
 public:
  // 6 个 CRUD override ...
 private:
  struct Entry {
    UploadRecord             record;
    std::vector<PartRecord>  parts;
    mutable std::mutex       parts_mu;
  };
  std::unordered_map<std::string, std::unique_ptr<Entry>> sessions_;
  std::shared_mutex sessions_mu_;
};
```
实现要点（迁自旧 SessionManager 的对应 CRUD 段）：
- `Create`：`utils::GenUuid()` + `utils::NowMs()`，写入 map，返回 upload_id。
- `Get`：shared_lock 查 map，拷出 record（不返回裸指针）。
- `AddPart`：shared_lock 取 entry + parts_mu，同 part_number 覆盖（保留 size 变更 warn 日志）。
- `ListParts`：shared_lock 取 entry + parts_mu，拷出 parts 副本（未排序）。
- `Remove`：unique_lock erase（幂等）。
- `RemoveExpired`：`(now - created_at_ms) > ttl_ms` 判断内联于此（原 IsExpired 逻辑）。

**不含** ValidatePartList / ComputeFinalETag / CompleteSession——已上移服务层。

---

## 改动 3：MultipartService 补业务规则（上移自 SessionManager）

`multipart_service.h` 私有方法：
```cpp
[[nodiscard]] ProxyStatus ValidateParts(const std::vector<PartRecord>& parts);
[[nodiscard]] std::string ComputeFinalETag(const std::vector<PartRecord>& parts);
```
`multipart_service.cpp`：
- `ValidateParts`：迁自旧 `ValidatePartList`——非空 + 升序无重复（允许间隙）。
- `ComputeFinalETag`：迁自旧 `ComputeFinalETag`——收集各 part etag → `utils::CombineETags`。
- `CompleteUpload` 重写为编排（原 CompleteSession 的四件事在这里显式串起来）：
  ```cpp
  CompleteResult MultipartService::CompleteUpload(
      const std::string& upload_id,
      const std::vector<CompleteMultipartUploadRequest_PartInfo>& client_parts) {
    UploadRecord rec;
    if (!index_->Get(upload_id, rec))
      return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM, "upload_id not found"), {}, {}, 0};
    std::vector<PartRecord> parts;
    index_->ListParts(upload_id, parts);
    std::sort(parts.begin(), parts.end(),
              [](auto& a, auto& b){ return a.part_number < b.part_number; });
    if (auto s = ValidateParts(parts); !s.ok()) return {s, {}, {}, 0};
    // client etag 比对（若提供）
    if (!client_parts.empty()) {
      if (client_parts.size() != parts.size())
        return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM, "part count mismatch"), {}, {}, 0};
      for (std::size_t i = 0; i < parts.size(); ++i)
        if (client_parts[i].part_number() != parts[i].part_number ||
            client_parts[i].etag() != parts[i].etag)
          return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM,
                  "part etag mismatch at part " + std::to_string(parts[i].part_number)), {}, {}, 0};
    }
    std::uint64_t size = 0; for (auto& p : parts) size += p.part_size;
    CompleteResult r;
    r.status = ProxyStatus::Ok();
    r.object_id = rec.bucket + "/" + rec.key;
    r.etag = ComputeFinalETag(parts);
    r.object_size = size;
    index_->Remove(upload_id);   // 成功后清理
    return r;
  }
  ```
- `MultipartService` 成员类型改为 `IUploadIndex* index_;`。
- `UploadPartGds/Ucx` 里 GetSessionPath 改为 `index_->Get(upload_id, rec)` 后读 `rec.path`。

---

## 改动 4：删旧文件 + 装配调整

- 删 `proxy/src/multipart/session_manager.{h,cpp}`。
- 删 `proxy/src/multipart/upload_session.h`。
- 接口层/main 创建 `InMemoryUploadIndex` 注入给 MultipartService（替换原 SessionManager*）。
- TTL 清理线程调用改为 `index_->RemoveExpired(kTtlMs)`。

## 改动 5：CMakeLists.txt
- 新增 `src/index/in_memory_upload_index.cpp`。
- 删除 `src/multipart/session_manager.cpp`。

---

## 验收（阶段 3）

**索引层纯被动**
- [ ] `IUploadIndex` 只有 6 个 CRUD，无校验/etag/比对
- [ ] `InMemoryUploadIndex` 不含 ValidatePartList/ComputeFinalETag
- [ ] 业务规则（ValidateParts/ComputeFinalETag/client 比对）全在 MultipartService

**可替换性**
- [ ] MultipartService 只依赖 `IUploadIndex*`，不知道内存/Mongo
- [ ] （思想验证）新写一个 MongoUploadIndex 只需实现接口，服务层零改动

**行为等价**
- [ ] part 升序无重复校验不变、final etag 算法不变、client etag 比对不变
- [ ] 成功后清理会话、TTL 过期清理 不变
- [ ] 现有测试全绿

## 交付物
1. `index/upload_index.h`（新，接口+记录）
2. `index/in_memory_upload_index.{h,cpp}`（新，mock）
3. `service/multipart_service.{h,cpp}`（补业务规则，改依赖 IUploadIndex*）
4. 删 `multipart/session_manager.*`、`multipart/upload_session.h`
5. CMakeLists.txt

完成后进入阶段 4（抽存储层 BackendGateway/BlockStorage，channel 下沉，清空 multipart/）。
