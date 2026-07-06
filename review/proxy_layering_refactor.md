# Proxy 分层重构提示词 — 接口层 / 服务层 / 索引层(mock) / 存储层

目标：把当前"上帝类" `ProxyControlPlaneService` 拆成 4 层，职责清晰、可单测、易拓展
（内存索引 → 后续 MongoDB；backend 传输可替换）。**依序落地：1 接口层 → 2 服务层
→ 3 索引层(先 mock) → 4 存储层**。本文件是提示词，不直接改源码。

---

## 0. 现状评估（为什么要分层）

`ProxyControlPlaneService` 一个 brpc service 类同时干 4 件事：
1. proto 解包 / error code 映射 / ClosureGuard（**接口层**职责）
2. 参数校验 + 单步转发编排 + multipart 流程编排（**服务层**职责）
3. 会话/part 元数据存取——且 `SessionManager` 里还混入 `ValidatePartList` /
   `ComputeFinalETag` / client etag 比对等**业务规则**（**索引层**职责被污染）
4. backend channel(SINGLE+POOLED) 管理、单步转发、block 切分（**存储层**职责）

坏味道：`CompleteSession` 把"读元数据 + 校验 part + 算 final etag + 比对 client etag"
四件事揉在一个函数；单步 handler 6 段 `if+SetFailed` 与转发、日志线性堆叠无法单测；
换索引后端需改 `SessionManager` 本体（无接口隔离）。

---

## 1. 目标分层架构

```
 client ──RPC──▶ [接口层] ProxyControlPlaneService (implements brpc Control)
                     │  仅：ClosureGuard、proto↔域对象、ProxyStatus→cntl/response
                     ▼
                 [服务层] SinglePutService / MultipartService
                     │  校验 + 编排；返回 ProxyStatus（域错误码）
            ┌────────┴────────┐
            ▼                 ▼
     [索引层] IUploadIndex   [存储层] BackendGateway / BlockStorage
     元数据 CRUD(纯被动)      backend RPC(SINGLE 单步 / POOLED block)
     └ InMemoryUploadIndex   GDS/UCX 各自独立方法
        (mock，后续 Mongo)
```

**目录（新）**
```
proxy/src/
  api/       proxy_control_plane_service.{h,cpp}   # 接口层(由 service/ 迁入)
  service/   single_put_service.{h,cpp}
             multipart_service.{h,cpp}
  index/     upload_index.h                        # 接口 + 记录结构体
             in_memory_upload_index.{h,cpp}        # mock 实现
  storage/   backend_gateway.{h,cpp}               # 单步转发
             block_storage.{h,cpp}                 # block 切分(由 multipart_put_handler 迁入)
  common/    status.h(新) errors.h utils.{h,cpp}
```
删除 `multipart/session_manager.*`、`multipart/upload_session.h`、`multipart/multipart_put_handler.*`
（内容拆入 index/ 与 storage/）。

---

## 2. 贯穿全局的约束（分层时不得破坏）

1. **GDS/UCX 链路不共享代码**：分层是纵向切职责，链路隔离是横向约束。每层内
   GDS 与 UCX 保持**独立方法**（`PutGds`/`PutUcx`、`UploadPartGds`/`UploadPartUcx`、
   `ForwardGdsPut`/`ForwardUcxPut`、`HandleGdsPart`/`HandleUcxPart`），**禁止**用一个内部
   分支函数把两条链路合并——改一条不得影响另一条。仅纯链路无关算法可共用（如
   `utils::CombineETags`、`utils::NowMs`）。
2. **brpc 单 service 约束**：一个 proto service 只能注册一个 C++ 实例。接口层仍是
   **唯一**的 `Control` 子类，内部委托给服务层对象；不拆成多个 service 子类。
3. **索引层是纯被动存储**：只做元数据 CRUD，**不含**任何业务规则（校验、etag 计算、
   client 比对全部上移到服务层）。这样内存 mock 与后续 Mongo 实现可无差别替换。
4. **既往 20 条规范**：注释 ≤1 行/块、无死代码、错误码用到再加、`[[nodiscard]]`、
   命名一致、RAII、不过度抽象等，沿用不回退。
5. **行为等价**：重构不改对外 RPC 语义（16MiB 限制、串行 block、part 升序无重复、
   final etag 算法、成功后清理会话、Abort 幂等）全部保持。

---

## 3. 域错误类型（先建，各层共用）

`proxy/src/common/status.h`（新）——服务层统一返回它，接口层负责映射到 brpc：

```cpp
#pragma once
#include <string>
#include <utility>
#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

// 服务层统一返回：code==0 表示成功，非 0 为 PROXY_ERR_*。接口层据此填 response
// 并 cntl->SetFailed；不把 brpc 概念泄漏进服务层。
struct ProxyStatus {
  int         code{0};
  std::string message;

  [[nodiscard]] bool ok() const { return code == 0; }
  static ProxyStatus Ok() { return {}; }
  static ProxyStatus Fail(int code, std::string msg) {
    return {code, std::move(msg)};
  }
};

}  // namespace us3_turbo::proxy
```

---

## 4. 第一层：接口层 (API Layer)

**位置**：`proxy/src/api/proxy_control_plane_service.{h,cpp}`（从 service/ 迁入）

**职责**：proto ↔ 域对象、brpc ClosureGuard、ProxyStatus → cntl/response。
**不做**：任何参数校验、业务编排。

### 依赖注入

构造函数：
```cpp
ProxyControlPlaneService(
    std::unique_ptr<SinglePutService> single_put_svc,
    std::unique_ptr<MultipartService> multipart_svc);
```
不持有 backend channel/stub（下沉到存储层）、不持有 SessionManager（下沉到索引层）。

### GdsPut/UcxPut 改造（单步接口）

**原：**427 行 cpp 里 `GdsPut` 有 6 段 `if+SetFailed` + backend 转发 + 日志，共 54 行。

**改后：**
```cpp
void ProxyControlPlaneService::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ClientProxyPutRequest* request,
    PutPathResult* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // 委托给服务层（单行域对象提取）。
  auto result = single_put_svc_->PutGds(
      request->bucket(), request->key(), request->object_size(),
      request->has_gds_source() ? &request->gds_source() : nullptr);

  // 映射 ProxyStatus → response。
  if (!result.status.ok()) {
    cntl->SetFailed(result.status.code, "%s", result.status.message.c_str());
    return;
  }
  response->set_ok(true);
  response->set_etag(result.etag);
  response->set_bytes_written(result.bytes_written);
  if (result.crc32c != 0) response->set_crc32c(result.crc32c);
}
```
`UcxPut` 同理（独立方法，调 `single_put_svc_->PutUcx(...)`）。

### Multipart 5 个接口

同样薄委托：
- `CreateMultipartUpload` → `multipart_svc_->CreateUpload(...)`
- `UploadPartGds` → `multipart_svc_->UploadPartGds(...)`
- `UploadPartUcx` → `multipart_svc_->UploadPartUcx(...)`（独立）
- `CompleteMultipartUpload` → `multipart_svc_->CompleteUpload(...)`
- `AbortMultipartUpload` → `multipart_svc_->AbortUpload(...)`

每个接口 ≤20 行：proto→域对象、调服务层、status→response。

---

## 5. 第二层：服务层 (Service Layer)

**位置**：`proxy/src/service/single_put_service.{h,cpp}` +
`proxy/src/service/multipart_service.{h,cpp}`

**职责**：参数校验 + 业务编排（单步转发 / multipart 流程）+ 返回 ProxyStatus。
**不做**：proto 解包、brpc SetFailed、不直接操作 brpc channel。

### SinglePutService

```cpp
// single_put_service.h
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "control_plane.pb.h"
#include "proxy/src/common/status.h"

namespace us3_turbo::proxy {
class BackendGateway;  // 前向声明

struct PutResult {
  ProxyStatus  status;
  std::string  etag;
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
};

class SinglePutService {
 public:
  explicit SinglePutService(BackendGateway* gateway);

  // GDS 单步上传：校验 + 转发。
  [[nodiscard]] PutResult PutGds(
      const std::string& bucket,
      const std::string& key,
      std::uint64_t object_size,
      const GdsSource* gds_source);

  // UCX 单步上传（独立方法，与 GDS 不共享代码）。
  [[nodiscard]] PutResult PutUcx(
      const std::string& bucket,
      const std::string& key,
      std::uint64_t object_size,
      const UcxSource* ucx_source);

 private:
  BackendGateway* gateway_;  // 不拥有，由 main 统一生命周期
};

}  // namespace us3_turbo::proxy
```

**实现**（`single_put_service.cpp`）：
- `PutGds`：6 段参数校验（bucket/key/size/16MB/gds_source），任一失败返回
  `ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM, ...)`；通过后调 `gateway_->ForwardGdsPut(...)`
  返回 PutResult。
- `PutUcx`：独立实现（不调 PutGds），校验逻辑各写一遍。

### MultipartService

持有两个下层指针：`IUploadIndex* index_`（索引层）+ `BlockStorage* block_storage_`（存储层）。

```cpp
// multipart_service.h
class MultipartService {
 public:
  MultipartService(IUploadIndex* index, BlockStorage* block_storage);

  // 创建：校验 path ∈ {GDS,UCX} → index_->Create(...) 返回 upload_id。
  [[nodiscard]] CreateResult CreateUpload(
      const std::string& bucket, const std::string& key, PutDataPath path);

  // GDS 上传 part：校验 upload_id/path/part_number/part_size/rdma_token →
  // block_storage_->PutPartGds(...) → index_->AddPart(...)。
  [[nodiscard]] UploadPartResult UploadPartGds(
      const std::string& request_id, const std::string& upload_id,
      std::uint32_t part_number, std::uint64_t part_size,
      const std::string& rdma_token);

  // UCX 上传 part（独立方法，不共享 GDS 代码）。
  [[nodiscard]] UploadPartResult UploadPartUcx(
      const std::string& request_id, const std::string& upload_id,
      std::uint32_t part_number, std::uint64_t part_size,
      std::uint64_t remote_addr, const std::string& packed_rkey,
      const std::string& client_ucx_addr);

  // 完成：index_->ListParts → 校验升序无重复 → (可选) 比对 client etag →
  // CombineETags 算 final → index_->Remove（成功后清理）。业务规则全在这层。
  [[nodiscard]] CompleteResult CompleteUpload(
      const std::string& upload_id,
      const std::vector<PartInfo>& client_parts);

  // 幂等：index_->Remove(upload_id)。
  [[nodiscard]] ProxyStatus AbortUpload(const std::string& upload_id);

 private:
  // part 校验 / final etag 计算从旧 SessionManager 上移到这里（服务层业务规则）。
  [[nodiscard]] ProxyStatus ValidateParts(const std::vector<PartRecord>& parts);
  [[nodiscard]] std::string ComputeFinalETag(const std::vector<PartRecord>& parts);

  IUploadIndex* index_;
  BlockStorage* block_storage_;
};
```

**关键迁移**：旧 `SessionManager::ValidatePartList` / `ComputeFinalETag` / client etag
比对循环，**从索引层上移到 MultipartService**（它们是业务规则，不是存储）。
`CompleteUpload` 编排：取 parts → ValidateParts → 比对 → ComputeFinalETag → Remove。

---

## 6. 第三层：索引层 (Index Layer) — 先 mock

**位置**：`proxy/src/index/upload_index.h`（接口）+ `in_memory_upload_index.{h,cpp}`（mock 实现）

**职责**：会话/part 元数据**纯被动 CRUD**。**不含**任何校验、etag 计算、client 比对。
**为什么先 mock**：接口隔离后，后续 MongoDB(`us3_minit`/`us3_partlist`) 实现同一接口即可
无差别替换，服务层零改动。

### 记录结构体（上移自 upload_session.h，去掉业务方法）

```cpp
// upload_index.h
struct PartRecord {
  std::uint32_t part_number{0};
  std::uint64_t part_size{0};
  std::string   etag;
  std::int64_t  upload_time_ms{0};
};

struct UploadRecord {
  std::string  upload_id;
  std::string  bucket;
  std::string  key;
  PutDataPath  path{PATH_NONE};
  std::int64_t created_at_ms{0};
};
```
注意：`TotalSize()`/`IsExpired()` 这类计算不再挂在记录上——`IsExpired` 判断逻辑
下沉进索引实现的 `RemoveExpired(ttl_ms)`；总大小由服务层累加。

### 接口

```cpp
class IUploadIndex {
 public:
  virtual ~IUploadIndex() = default;

  // 创建会话，返回生成的 upload_id。
  [[nodiscard]] virtual std::string Create(
      const std::string& bucket, const std::string& key, PutDataPath path) = 0;

  // 读会话（不存在返回 false）。纯读，不含业务判断。
  [[nodiscard]] virtual bool Get(
      const std::string& upload_id, UploadRecord& out) = 0;

  // 追加/覆盖 part（同 part_number 覆盖）。不存在返回 false。
  [[nodiscard]] virtual bool AddPart(
      const std::string& upload_id, const PartRecord& part) = 0;

  // 列出某会话所有 part（未排序，排序/校验由服务层做）。
  [[nodiscard]] virtual bool ListParts(
      const std::string& upload_id, std::vector<PartRecord>& out) = 0;

  // 删除会话（幂等）。
  virtual void Remove(const std::string& upload_id) = 0;

  // 删除过期会话（TTL 清理线程调用）。
  virtual void RemoveExpired(std::int64_t ttl_ms) = 0;
};
```

### InMemoryUploadIndex（mock）

- 内部 `unordered_map<string, unique_ptr<记录+parts+parts_mu>>` + `shared_mutex`
  （沿用旧 SessionManager 的并发模型）。
- 只实现上述 6 个 CRUD，**无** ValidatePartList / ComputeFinalETag（已上移服务层）。
- `Create` 用 `utils::GenUuid()`；时间戳用 `utils::NowMs()`。
- TTL 清理线程**不放这里**——线程属接口层/main 生命周期，调 `index_->RemoveExpired(ttl)`。

---

## 7. 第四层：存储层 (Storage Layer)

**位置**：`proxy/src/storage/backend_gateway.{h,cpp}`（单步转发）+
`proxy/src/storage/block_storage.{h,cpp}`（block 切分，迁自 multipart_put_handler）

**职责**：管理 backend channel/stub、执行 backend RPC。**不做**参数校验、业务编排。

### BackendGateway（单步转发，SINGLE channel）

```cpp
class BackendGateway {
 public:
  // 内部建 SINGLE channel + Control_Stub；endpoint 空则 stub 为空。
  BackendGateway(const std::string& backend_endpoint, int timeout_ms);

  [[nodiscard]] bool available() const { return stub_ != nullptr; }

  // GDS 单步转发（把整个 ClientProxyPutRequest 透传给 backend GdsPut）。
  [[nodiscard]] PutResult ForwardGdsPut(const ClientProxyPutRequest& request);
  // UCX 单步转发（独立方法，不共享 GDS 代码）。
  [[nodiscard]] PutResult ForwardUcxPut(const ClientProxyPutRequest& request);

 private:
  int timeout_ms_;
  std::shared_ptr<brpc::Channel>       channel_;
  std::unique_ptr<Control_Stub>        stub_;
};
```
把旧 `GdsPut`/`UcxPut` 里的 backend 转发段（bcntl + backend_stub_->GdsPut + Failed 判断）
迁进 `ForwardGdsPut`/`ForwardUcxPut`，返回 PutResult（backend 不可用 → status Fail）。

### BlockStorage（block 切分，POOLED channel）

迁自 `multipart_put_handler.{h,cpp}`——内容基本照搬，仅重命名 + 自持 channel：

```cpp
class BlockStorage {
 public:
  // 内部建 POOLED channel + BackendDataPlane_Stub。
  BlockStorage(const std::string& backend_endpoint, int timeout_ms,
               std::uint64_t block_size = 4ULL * 1024 * 1024);

  [[nodiscard]] bool available() const { return stub_ != nullptr; }

  // 保留原 HandleGdsPart / HandleUcxPart 独立实现 + 串行 for 循环 + Aggregate。
  [[nodiscard]] PartResult PutPartGds(...);   // 原 HandleGdsPart
  [[nodiscard]] PartResult PutPartUcx(...);   // 原 HandleUcxPart（独立）
 private:
  // SplitToBlocks / CallBackendPutBlockGds / CallBackendPutBlockUcx / Aggregate
  // 全部照搬，串行 block 逻辑与 GDS/UCX 独立性不变。
  ...
};
```
**注意**：channel 从接口层构造函数下沉到这两个存储类各自持有——SINGLE 归
BackendGateway，POOLED 归 BlockStorage，各自 RAII 管理，接口层不再碰 brpc channel。

---

## 8. main.cpp 装配（依赖注入根）

分层后由 main 组装对象图（自底向上），统一生命周期：

```cpp
// 存储层
auto gateway = std::make_unique<BackendGateway>(backend_endpoint, timeout_ms);
auto block_storage = std::make_unique<BlockStorage>(backend_endpoint, timeout_ms);
// 索引层（mock）
auto index = std::make_unique<InMemoryUploadIndex>();
// 服务层
auto single_svc = std::make_unique<SinglePutService>(gateway.get());
auto multipart_svc = std::make_unique<MultipartService>(index.get(), block_storage.get());
// 接口层（持有 service，service 持有下层裸指针）
auto service = std::make_unique<ProxyControlPlaneService>(
    std::move(single_svc), std::move(multipart_svc));
// TTL 清理线程：main 或接口层持有，定时调 index->RemoveExpired(kTtlMs)。
```
生命周期顺序：gateway/block_storage/index 最长命（main 栈底），service 次之，接口层最上。
裸指针注入（下层不拥有），析构逆序天然安全。TTL 线程仍用 condition_variable + join。

---

## 9. 落地顺序（严格依序，每步可编译）

**步骤 1 — 接口层就位（先不拆下层）**
- 建 `common/status.h`。
- 把 `service/proxy_control_plane_service.*` 迁到 `api/`。
- 暂时保持它内部直接持有现有 SessionManager/MultipartPutHandler/channel，**先只加
  status.h 与目录迁移**，确保编译通过。

**步骤 2 — 抽服务层**
- 建 `service/single_put_service.*` + `service/multipart_service.*`。
- 把校验 + 编排逻辑从接口层搬进服务层，接口层改薄委托。
- 业务规则（ValidateParts/ComputeFinalETag/client 比对）从 SessionManager 上移到
  MultipartService。

**步骤 3 — 抽索引层（mock）**
- 建 `index/upload_index.h`（接口 + 记录）+ `index/in_memory_upload_index.*`。
- 把 SessionManager 的纯 CRUD 迁进 InMemoryUploadIndex，实现 IUploadIndex。
- MultipartService 改为持 `IUploadIndex*`。删除 `multipart/session_manager.*`、
  `multipart/upload_session.h`。

**步骤 4 — 抽存储层**
- 建 `storage/backend_gateway.*`（单步转发迁自接口层构造 + Forward 段）。
- 建 `storage/block_storage.*`（迁自 `multipart_put_handler.*`，重命名 Put*）。
- channel 下沉到存储类自持。删除 `multipart/multipart_put_handler.*`、清空 `multipart/`。
- main.cpp 按 §8 装配。

每步结束都应能编译 + 跑通现有 multipart 测试，行为不变。

---

## 10. CMakeLists.txt 同步

`proxy/CMakeLists.txt` 源文件列表更新为：
```cmake
src/main.cpp
src/common/utils.cpp
src/index/in_memory_upload_index.cpp
src/storage/backend_gateway.cpp
src/storage/block_storage.cpp
src/service/single_put_service.cpp
src/service/multipart_service.cpp
src/api/proxy_control_plane_service.cpp
```
删除 `src/multipart/session_manager.cpp`、`src/multipart/multipart_put_handler.cpp`、
旧 `src/service/proxy_control_plane_service.cpp` 路径。

---

## 11. 验收清单

**分层职责**
- [ ] 接口层每个 handler ≤20 行，只有 ClosureGuard + proto↔域 + status→response，无校验/编排
- [ ] 服务层不含 `#include <brpc/...>`、无 SetFailed、无 proto Closure
- [ ] 索引层无校验/etag/client 比对，只有 6 个 CRUD
- [ ] 存储层各自持 channel，接口层不再出现 brpc::Channel

**链路隔离（横向约束不破）**
- [ ] 每层 GDS/UCX 均为独立方法，无合并的内部分支函数
- [ ] 注释掉 `PutGds`/`UploadPartGds`/`ForwardGdsPut`/`PutPartGds` 任一，UCX 链路仍编译
- [ ] 反之注释 UCX 方法，GDS 链路仍编译

**行为等价**
- [ ] 单步 16MiB 边界、multipart part 升序无重复、final etag、成功清理、Abort 幂等 全部不变
- [ ] 现有 multipart 测试全绿

**可拓展性（分层目的）**
- [ ] 新增一个 `MongoUploadIndex : IUploadIndex` 只需实现接口，服务层零改动
- [ ] 替换 BackendGateway 传输实现不影响服务层

---

## 12. 一句话总结

纵向切 4 层（接口/服务/索引/存储），横向保持 GDS/UCX 不共享；索引层先 mock、
接口隔离到位，后续换 MongoDB / 换传输都只动一层。依 §9 四步依序落地，每步可编译可测。
