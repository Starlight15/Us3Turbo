# Proxy 分层重构 — 阶段 2：抽出服务层（SinglePutService / MultipartService）

**前置**：阶段 1 完成（status.h 就位、接口层在 api/）。

**目标**：把校验 + 编排逻辑从接口层下沉到服务层；接口层改薄委托。业务规则
（part 校验 / final etag / client etag 比对）从 SessionManager 上移到 MultipartService。
**此阶段索引层/存储层仍是旧的 SessionManager / MultipartPutHandler**——只在服务层
之下再包一层，尚未替换实现。

**范围**：4 个新文件（service 层 h/cpp × 2）+ 接口层改薄 + 装配。

---

## 约束（不得破坏）

- GDS/UCX 每个方法独立：`PutGds`/`PutUcx`、`UploadPartGds`/`UploadPartUcx` 各写一遍，
  **不许**合并成一个带 path 分支的内部函数。
- 服务层**不含** `#include <brpc/...>`、无 `SetFailed`、无 proto `Closure`。
- 行为等价：16MiB 限制、part 升序无重复、final etag 算法全部不变。

---

## 改动 1：service/single_put_service.h

```cpp
#pragma once
#include <cstdint>
#include <string>
#include "control_plane.pb.h"
#include "proxy/src/common/status.h"

namespace us3_turbo::proxy {

struct PutResult {
  ProxyStatus   status;
  std::string   etag;
  std::uint32_t crc32c{0};
  std::uint64_t bytes_written{0};
};

// 单步上传服务：校验参数后转发给 backend。GDS/UCX 各自独立方法。
class SinglePutService {
 public:
  // backend_stub 由外部（接口层/main）持有，本类不拥有；endpoint 空时 stub 为 nullptr。
  SinglePutService(::us3_turbo::proxy::Control_Stub* backend_stub, int timeout_ms);

  [[nodiscard]] PutResult PutGds(const ClientProxyPutRequest& request);
  [[nodiscard]] PutResult PutUcx(const ClientProxyPutRequest& request);

 private:
  ::us3_turbo::proxy::Control_Stub* backend_stub_;
  int timeout_ms_;
};

}  // namespace us3_turbo::proxy
```

> 注：阶段 2 先让服务层直接持 backend_stub_（存储层阶段 4 再换成 BackendGateway*）。
> 传整个 request 进来，避免逐字段拆——转发本就要透传整个 proto。

## 改动 2：service/single_put_service.cpp

`PutGds` 把旧接口层 `GdsPut` 的 6 段校验 + backend 转发迁进来：
```cpp
constexpr std::uint64_t kMaxUploadBytes = 16ULL * 1024 * 1024;

PutResult SinglePutService::PutGds(const ClientProxyPutRequest& request) {
  if (request.bucket().empty() || request.key().empty())
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM, "missing bucket or key"), {}, 0, 0};
  if (request.object_size() == 0)
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM, "object_size must be > 0 for GDS PUT"), {}, 0, 0};
  if (request.object_size() > kMaxUploadBytes)
    return {ProxyStatus::Fail(PROXY_ERR_INVALID_PARAM,
            "object_size exceeds 16MiB single-step limit; use multipart"), {}, 0, 0};
  if (request.path() != PATH_GDS)
    return {ProxyStatus::Fail(PROXY_ERR_PATH_NOT_SUPPORTED, "GdsPut requires PATH_GDS"), {}, 0, 0};
  if (!request.has_gds_source())
    return {ProxyStatus::Fail(PROXY_ERR_MISSING_SOURCE, "GdsPut requires gds_source"), {}, 0, 0};
  if (backend_stub_ == nullptr)
    return {ProxyStatus::Fail(PROXY_ERR_BACKEND_UNAVAILABLE, "no backend channel"), {}, 0, 0};

  brpc::Controller bcntl;
  bcntl.set_timeout_ms(timeout_ms_);
  PutPathResult bresp;
  backend_stub_->GdsPut(&bcntl, &request, &bresp, nullptr);
  if (bcntl.Failed())
    return {ProxyStatus::Fail(PROXY_ERR_BACKEND_RPC,
            std::string("backend GdsPut failed: ") + bcntl.ErrorText()), {}, 0, 0};

  return {ProxyStatus::Ok(), bresp.etag(), bresp.crc32c(), bresp.bytes_written()};
}
```
`PutUcx` 独立实现（校验 path==PATH_UCX、has_ucx_source，转发 `backend_stub_->UcxPut`）。

---

## 改动 3：service/multipart_service.h

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "control_plane.pb.h"
#include "proxy/src/common/status.h"
#include "proxy/src/multipart/session_manager.h"       // 阶段 2 仍用旧的
#include "proxy/src/multipart/multipart_put_handler.h"  // 阶段 2 仍用旧的

namespace us3_turbo::proxy {

struct CreateResult    { ProxyStatus status; std::string upload_id; };
struct UploadPartResult{ ProxyStatus status; std::string etag;
                         std::uint32_t crc32c{0}; std::uint64_t bytes_written{0}; };
struct CompleteResult  { ProxyStatus status; std::string object_id, etag;
                         std::uint64_t object_size{0}; };

class MultipartService {
 public:
  MultipartService(SessionManager* index, MultipartPutHandler* block_storage);

  [[nodiscard]] CreateResult CreateUpload(
      const std::string& bucket, const std::string& key, PutDataPath path);
  [[nodiscard]] UploadPartResult UploadPartGds(
      const std::string& request_id, const std::string& upload_id,
      std::uint32_t part_number, std::uint64_t part_size,
      const std::string& rdma_token);
  [[nodiscard]] UploadPartResult UploadPartUcx(
      const std::string& request_id, const std::string& upload_id,
      std::uint32_t part_number, std::uint64_t part_size,
      std::uint64_t remote_addr, const std::string& packed_rkey,
      const std::string& client_ucx_addr);
  [[nodiscard]] CompleteResult CompleteUpload(
      const std::string& upload_id,
      const std::vector<CompleteMultipartUploadRequest_PartInfo>& client_parts);
  [[nodiscard]] ProxyStatus AbortUpload(const std::string& upload_id);

 private:
  SessionManager*      index_;          // 阶段 3 换 IUploadIndex*
  MultipartPutHandler* block_storage_;  // 阶段 4 换 BlockStorage*
};

}  // namespace us3_turbo::proxy
```

## 改动 4：service/multipart_service.cpp（编排 + 校验）

- `CreateUpload`：校验 path ∈ {GDS,UCX} → `index_->CreateSession(...)`。
- `UploadPartGds`：GetSessionPath → 校验 path==GDS/part_number/part_size/rdma_token
  → `block_storage_->HandleGdsPart(...)` → 构造 PartMetadata → `index_->AddPart(...)`。
- `UploadPartUcx`：独立实现（校验 ucx 三字段，调 HandleUcxPart）。
- `CompleteUpload`：调 `index_->CompleteSession(...)`（阶段 2 暂时仍用旧签名，业务规则
  上移留到阶段 3 索引层被动化时一起做）成功后 `index_->CleanupSession`。
- `AbortUpload`：`index_->CleanupSession` → Ok（幂等）。

> 阶段 2 的重点是**把编排从接口层挪到服务层**；SessionManager 内部业务规则的剥离
> 放到阶段 3（换 IUploadIndex 接口时一次做干净），避免本阶段改动面过大。

---

## 改动 5：接口层改薄委托

`api/proxy_control_plane_service.h`：
- 构造函数改为持有 `std::unique_ptr<SinglePutService>` + `std::unique_ptr<MultipartService>`。
- 保留 backend channel/stub、SessionManager、MultipartPutHandler 成员（阶段 2 仍由接口层
  创建并注入给 service；阶段 3/4 再下沉）。
- TTL 清理线程保留。

`api/proxy_control_plane_service.cpp` 每个 handler 改成薄委托（≤20 行），例：
```cpp
void ProxyControlPlaneService::GdsPut(..., const ClientProxyPutRequest* request,
                                      PutPathResult* response, Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  auto r = single_put_svc_->PutGds(*request);
  if (!r.status.ok()) { cntl->SetFailed(r.status.code, "%s", r.status.message.c_str()); return; }
  response->set_ok(true);
  response->set_etag(r.etag);
  response->set_bytes_written(r.bytes_written);
  if (r.crc32c != 0) response->set_crc32c(r.crc32c);
}
```
6 个 handler（GdsPut/UcxPut/Create/UploadPartGds/UploadPartUcx/Complete/Abort）全部照此薄化。

---

## 改动 6：CMakeLists.txt

新增：
```cmake
src/service/single_put_service.cpp
src/service/multipart_service.cpp
```

---

## 验收（阶段 2）

**分层职责**
- [ ] 服务层 h/cpp 无 `SetFailed`、无 `Closure`、无 proto RpcController
- [ ] 接口层每个 handler ≤20 行，只做 ClosureGuard + 委托 + status→response

**链路隔离**
- [ ] PutGds/PutUcx、UploadPartGds/UploadPartUcx 各自独立，无合并分支函数
- [ ] 注释掉 PutGds 函数体，UCX 链路仍编译

**行为等价**
- [ ] 单步 16MiB 边界、multipart 全流程、final etag 不变
- [ ] 现有测试全绿

## 交付物
1. `service/single_put_service.{h,cpp}`（新）
2. `service/multipart_service.{h,cpp}`（新）
3. `api/proxy_control_plane_service.{h,cpp}`（改薄）
4. CMakeLists.txt

完成后进入阶段 3（抽索引层 IUploadIndex + InMemoryUploadIndex，业务规则上移）。
