# Proxy 分层重构 — 阶段 4：抽出存储层（BackendGateway / BlockStorage）+ 最终装配

**前置**：阶段 3 完成（索引层就位，业务规则已上移服务层）。

**目标**：把 backend channel/stub 管理 + RPC 执行下沉到存储层——单步转发归
`BackendGateway`（SINGLE channel），block 切分归 `BlockStorage`（POOLED channel，
迁自 multipart_put_handler）。接口层彻底不碰 brpc channel。main 按依赖注入装配。
删除 `multipart/multipart_put_handler.*`，清空 `multipart/` 目录。

**范围**：4 个新文件（storage 层 h/cpp × 2）+ 服务层改依赖 + main 装配 + 删旧文件。

---

## 约束

- GDS/UCX 每层独立方法：`ForwardGdsPut`/`ForwardUcxPut`、`PutPartGds`/`PutPartUcx`
  各写一遍，**不许**合并。
- 存储层各自持有 channel（RAII），接口层不再出现 `brpc::Channel`。
- 串行 block 逻辑（block 数 ≤4）不变、Aggregate/SplitToBlocks 照搬。

---

## 改动 1：storage/backend_gateway.{h,cpp}（单步转发，SINGLE）

```cpp
// backend_gateway.h
#pragma once
#include <memory>
#include <string>
#include <brpc/channel.h>
#include "control_plane.pb.h"
#include "proxy/src/service/single_put_service.h"  // PutResult

namespace us3_turbo::proxy {

// 单步转发到 backend：自持 SINGLE channel + Control_Stub。GDS/UCX 各独立方法。
class BackendGateway {
 public:
  BackendGateway(const std::string& backend_endpoint, int timeout_ms);
  [[nodiscard]] bool available() const { return stub_ != nullptr; }

  [[nodiscard]] PutResult ForwardGdsPut(const ClientProxyPutRequest& request);
  [[nodiscard]] PutResult ForwardUcxPut(const ClientProxyPutRequest& request);

 private:
  int timeout_ms_;
  std::shared_ptr<brpc::Channel>       channel_;
  std::unique_ptr<Control_Stub>        stub_;
};

}  // namespace us3_turbo::proxy
```
`backend_gateway.cpp`：
- 构造：迁自旧接口层构造函数的 SINGLE channel 创建段（endpoint 空 → stub_ 保持空）。
- `ForwardGdsPut`：迁自 SinglePutService::PutGds 里的 backend 转发段（bcntl + GdsPut +
  Failed 判断），返回 PutResult。**校验仍留在 SinglePutService**，Gateway 只管转发。
- `ForwardUcxPut`：独立实现（转发 `stub_->UcxPut`）。

**SinglePutService 改动**：不再自持 backend_stub_，改持 `BackendGateway* gateway_`；
`PutGds` 校验通过后调 `gateway_->ForwardGdsPut(request)`（含 available() 判断）。

## 改动 2：storage/block_storage.{h,cpp}（block 切分，POOLED）

迁自 `multipart/multipart_put_handler.{h,cpp}`，重命名 + 自持 channel：
- 类名 `MultipartPutHandler` → `BlockStorage`。
- 方法 `HandleGdsPart` → `PutPartGds`，`HandleUcxPart` → `PutPartUcx`（签名不变）。
- 构造函数改为 `BlockStorage(const std::string& endpoint, int timeout_ms, block_size=4MB)`，
  内部自建 POOLED channel + BackendDataPlane_Stub（迁自旧接口层构造的 block channel 段）。
- 私有 `SplitToBlocks`/`CallBackendPutBlockGds`/`CallBackendPutBlockUcx`/`Aggregate`
  **原样照搬**（串行 for 循环、GDS/UCX 独立、CombineETags 汇总不变）。
- 加 `[[nodiscard]] bool available() const { return stub_ != nullptr; }`。

**MultipartService 改动**：成员 `MultipartPutHandler* block_storage_` 类型名改为
`BlockStorage* block_storage_`，方法调用 `HandleGdsPart`→`PutPartGds` 等重命名。

---

## 改动 3：接口层瘦身

`api/proxy_control_plane_service.h`：
- 删除成员：`backend_channel_`、`backend_stub_`、`backend_block_channel_`、
  `backend_block_stub_`、`session_manager_`、`put_handler_`。
- 只保留：`std::unique_ptr<SinglePutService>`、`std::unique_ptr<MultipartService>`、
  TTL 清理线程相关（thread/mutex/cv/bool）+ 一个 `IUploadIndex*` 供清理线程调用
  （或把清理线程也移交 main，见改动 4 二选一）。
- 构造函数签名：`ProxyControlPlaneService(unique_ptr<SinglePutService>,
  unique_ptr<MultipartService>, IUploadIndex* index_for_cleanup)`。
- 头注释更新：删掉 backend channel 线程安全说明段（channel 已下沉存储层）。

## 改动 4：main.cpp 装配（依赖注入根）

自底向上组装，统一生命周期：
```cpp
// 存储层（各自持 channel）
auto gateway       = std::make_unique<BackendGateway>(backend_endpoint, timeout_ms);
auto block_storage = std::make_unique<BlockStorage>(backend_endpoint, timeout_ms);
// 索引层（mock）
auto index = std::make_unique<InMemoryUploadIndex>();
// 服务层
auto single_svc    = std::make_unique<SinglePutService>(gateway.get(), timeout_ms);
auto multipart_svc = std::make_unique<MultipartService>(index.get(), block_storage.get());
// 接口层
ProxyControlPlaneService service(std::move(single_svc), std::move(multipart_svc), index.get());
```
生命周期顺序：gateway/block_storage/index 在 main 栈底最长命，service 引用其裸指针，
析构逆序天然安全。TTL 线程仍用 condition_variable + join（在接口层或 main 持有，
定时 `index->RemoveExpired(kTtlMs)`）。

## 改动 5：删旧文件 + 清空 multipart/
- 删 `proxy/src/multipart/multipart_put_handler.{h,cpp}`。
- `proxy/src/multipart/` 目录此时应为空 → 删除目录。

## 改动 6：CMakeLists.txt
最终源文件列表：
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
删除 `src/multipart/multipart_put_handler.cpp`。

---

## 验收（阶段 4 = 全局收尾）

**分层职责**
- [ ] 接口层无 `brpc::Channel`/stub 成员，无校验/编排
- [ ] 存储层各自持 channel（SINGLE 归 Gateway，POOLED 归 BlockStorage）
- [ ] 服务层依赖 `BackendGateway*` / `BlockStorage*` / `IUploadIndex*`，不建 channel

**链路隔离（横向约束不破）**
- [ ] 注释 ForwardGdsPut / PutPartGds 任一，UCX 链路仍编译
- [ ] 注释 ForwardUcxPut / PutPartUcx 任一，GDS 链路仍编译

**行为等价**
- [ ] 单步 16MiB、multipart 全流程、串行 block、final etag、成功清理、Abort 幂等 全不变
- [ ] 现有测试全绿

**可拓展性（最终目标验证）**
- [ ] 新增 `MongoUploadIndex : IUploadIndex` 只需实现接口，服务层零改动
- [ ] 替换 BackendGateway/BlockStorage 传输实现不影响服务层
- [ ] `multipart/` 目录已删除

## 交付物
1. `storage/backend_gateway.{h,cpp}`（新）
2. `storage/block_storage.{h,cpp}`（新，迁自 multipart_put_handler）
3. `service/single_put_service.*` + `multipart_service.*`（改依赖存储层指针）
4. `api/proxy_control_plane_service.*`（删 channel 成员，瘦身）
5. `main.cpp`（依赖注入装配）
6. 删 `multipart/multipart_put_handler.*` + 清空 multipart/ 目录
7. CMakeLists.txt 最终列表

---

## 全 4 阶段完成后的最终目录
```
proxy/src/
  api/       proxy_control_plane_service.{h,cpp}
  service/   single_put_service.{h,cpp}  multipart_service.{h,cpp}
  index/     upload_index.h  in_memory_upload_index.{h,cpp}
  storage/   backend_gateway.{h,cpp}  block_storage.{h,cpp}
  common/    status.h  errors.h  utils.{h,cpp}
  main.cpp
```
