# Proxy 端全量修改提示词

依据 `proxy_review.md`，落实全部改进项。分为四组：**死代码清理**、**UB/隐患根治**、**架构增强**、**s3 兼容**。
约束：只改现有文件（proto/client 除 A3 需要外，尽量不新建）；GDS/UCX 链路逻辑保持独立、不互相耦合。

---

## 组一：死代码清理（R1/R2/R3/R4/R5/R6/R7）

### R1 删 `HexVal` — `proxy/src/common/utils.cpp`
删除匿名 namespace 内 `HexVal`（line 14-20），全项目无调用。

### R3 删 `ErrorMessage()` 与两个未用错误码 — `proxy/src/common/errors.h`
- 删 `PROXY_ERR_UNSUPPORTED_PATH`(13001)、`PROXY_ERR_NOT_IMPLEMENTED`(90001)
- 删 `ErrorMessage()` 函数（25-36）及其 `#include <string_view>`（若无其他用）
- 保留实际使用的 5 个码：`INVALID_PARAM` / `BACKEND_UNAVAILABLE` / `BACKEND_RPC` / `PATH_NOT_SUPPORTED` / `MISSING_SOURCE`
- 更新错误码分段注释（去掉 13001/90001 段说明）

### R2 删死成员 `gateway_id_` — `proxy_control_plane_service.{h,cpp}` + `main.cpp`
- 头：删 `std::string gateway_id_;`（89）、构造参数 `std::string gateway_id`
- 头注释（39）：`gateway_id_/backend_endpoint_ 构造后只读` → 改为 `backend_* 构造后只读`
- cpp：构造函数签名去掉 `gateway_id`，初始化列表删 `gateway_id_(...)`
- main.cpp：`ProxyControlPlaneService service(FLAGS_backend_endpoint, FLAGS_backend_timeout_ms)`；
  删 `DEFINE_string(gateway_id, ...)`（14）
> 决策：直接删除，不保留（proxy 标识后续如需再引入）。

### R6 `backend_endpoint_` 降为局部 — `proxy_control_plane_service.{h,cpp}`
`backend_endpoint_` 仅构造期使用。删成员，构造函数直接用参数 `backend_endpoint`：
- 头：删 `std::string backend_endpoint_;`（92）
- cpp：构造体内 `backend_endpoint_` 全部改用形参 `backend_endpoint`
- `backend_timeout_ms_` **保留成员**（handler 122/180 仍用）
> 头注释（39）相应改为"backend_timeout_ms_ 构造后只读"。

### R4 UUID 种子修正 — `proxy/src/common/utils.cpp`
`GenUuid`（30-33）种子去掉无意义左移：
```cpp
static thread_local std::mt19937_64 rng{
    std::random_device{}() ^
    static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
```

### R5 `GenUuid` 注释精简 — `proxy/src/common/utils.cpp`
28-29、36 多行注释压到 ≤1 行/块（req #1）。保留"UUID v4，RFC 4122 version/variant"一行即可。

### R7 main flag 注释 — `proxy/src/main.cpp`
`--backend_endpoint`（15-16）注释 `proxy forwards GdsPut here` → `backend data plane endpoint (GdsPut/UcxPut/PutBlock)`。

---

## 组二：UB / 隐患根治（A1 / L1）

### A1 清理线程改 join + condition_variable — `proxy_control_plane_service.{h,cpp}`

**头文件**：成员区替换
```cpp
// 后台 TTL 清理线程（析构 join，避免 detach 后访问已析构成员）。
std::thread             cleanup_thread_;
std::mutex              cleanup_mu_;
std::condition_variable cleanup_cv_;
bool                    stop_cleanup_{false};   // cleanup_mu_ 保护
```
加 `#include <condition_variable>`、`#include <mutex>`（若未含）。

**cpp 构造函数**：清理线程改为
```cpp
cleanup_thread_ = std::thread([this]() {
  constexpr std::int64_t kTtlMs = 3LL * 24 * 3600 * 1000;
  constexpr auto kScanInterval = std::chrono::hours(1);
  std::unique_lock lock(cleanup_mu_);
  while (!stop_cleanup_) {
    if (cleanup_cv_.wait_for(lock, kScanInterval,
                             [this] { return stop_cleanup_; })) {
      break;  // 被析构唤醒
    }
    lock.unlock();
    session_manager_.CleanupExpiredSessions(kTtlMs);
    lock.lock();
  }
});
// 不再 detach
```

**cpp 析构函数**：
```cpp
ProxyControlPlaneService::~ProxyControlPlaneService() {
  {
    std::lock_guard lock(cleanup_mu_);
    stop_cleanup_ = true;
  }
  cleanup_cv_.notify_all();
  if (cleanup_thread_.joinable()) cleanup_thread_.join();
}
```
> `stop_cleanup_` 从 `std::atomic<bool>` 改为普通 bool（由 cleanup_mu_ 保护）；删 `#include <atomic>`（若无其他用）。

### L1 消除 `GetSession` 锁外裸指针 — `session_manager.{h,cpp}` + `proxy_control_plane_service.cpp`

在 SessionManager 增加锁内取 path 的接口，UploadPart 不再持裸指针：

**session_manager.h**：加
```cpp
/** @brief 锁内读取会话 path；会话不存在返回 false。 */
[[nodiscard]] bool GetSessionPath(const std::string& upload_id,
                                  ::us3_turbo::proxy::PutDataPath& out_path);
```
> 保留 `GetSession`（若他处仍用）；若清理后无调用者，一并删 `GetSession`。

**session_manager.cpp**：
```cpp
bool SessionManager::GetSessionPath(const std::string& upload_id,
                                    ::us3_turbo::proxy::PutDataPath& out_path) {
  std::shared_lock lock(sessions_mu_);
  auto it = sessions_.find(upload_id);
  if (it == sessions_.end()) return false;
  out_path = it->second->path;
  return true;
}
```

**proxy_control_plane_service.cpp** — `UploadPartGds`（239-252）改：
```cpp
::us3_turbo::proxy::PutDataPath path{};
if (!session_manager_.GetSessionPath(request->upload_id(), path)) {
  response->set_ok(false);
  response->set_error_message("upload_id not found");
  cntl->SetFailed(PROXY_ERR_INVALID_PARAM, "upload_id not found");
  return;
}
if (path != ::us3_turbo::proxy::PATH_GDS) {
  response->set_ok(false);
  response->set_error_message("session path is not PATH_GDS");
  cntl->SetFailed(PROXY_ERR_PATH_NOT_SUPPORTED, "session path is not PATH_GDS");
  return;
}
```
`UploadPartUcx`（305-318）同理改 `PATH_UCX`。后续 `AddPart` 已按 upload_id 重查，无需裸指针。

---

## 组三：架构增强（A2）

### A2 block channel 独立 + POOLED — `proxy_control_plane_service.{h,cpp}`

**头文件**：加独立 block channel 成员（放在 backend_block_stub_ 之前）
```cpp
std::shared_ptr<brpc::Channel> backend_block_channel_;  // block 级独立连接池
std::unique_ptr<::us3_turbo::proxy::BackendDataPlane_Stub> backend_block_stub_;
```

**cpp 构造函数**：单步 channel 保持 SINGLE；新建 block channel 用 POOLED
```cpp
auto block_channel = std::make_shared<brpc::Channel>();
brpc::ChannelOptions block_opts;
block_opts.timeout_ms = backend_timeout_ms;
block_opts.connection_type = brpc::CONNECTION_TYPE_POOLED;
if (block_channel->Init(backend_endpoint.c_str(), nullptr, &block_opts) != 0) {
  spdlog::warn("proxy: failed to init backend block channel; multipart disabled");
} else {
  backend_block_channel_ = std::move(block_channel);
  backend_block_stub_ = std::make_unique<::us3_turbo::proxy::BackendDataPlane_Stub>(
      backend_block_channel_.get());
  put_handler_ = std::make_unique<MultipartPutHandler>(
      backend_block_stub_.get(), backend_timeout_ms);
}
```
> 成员析构逆序保证：put_handler_ 先析构（释放对 stub 的裸引用），再 stub，再 channel。
> 确认头文件成员声明顺序为 channel → stub → put_handler_。

---

## 组四：s3 兼容（A3 / L2 / L3 / L4）

### A3 新增 `AbortMultipartUpload`

**proto/control_plane.proto** — 加消息 + Control RPC：
```protobuf
message AbortMultipartUploadRequest {
  string request_id = 1;
  string upload_id  = 2;
}
message AbortMultipartUploadResponse {
  bool   ok            = 1;
  string error_message = 2;
}
// service Control 内追加：
rpc AbortMultipartUpload(AbortMultipartUploadRequest)
    returns (AbortMultipartUploadResponse);
```

**proxy_control_plane_service.{h,cpp}** — 加 override handler：
```cpp
void AbortMultipartUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo::proxy::AbortMultipartUploadRequest* request,
    ::us3_turbo::proxy::AbortMultipartUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  session_manager_.CleanupSession(request->upload_id());  // 幂等，不存在也 ok
  response->set_ok(true);
  spdlog::info("AbortMultipartUpload: upload={}", request->upload_id());
}
```
**client 侧一并加**（与 CreateMultipartUpload 同构）：

`client/src/rpc/proxy_rpc.h` — 声明：
```cpp
/** @brief 终止分段上传，proxy 清理会话（幂等）。 */
[[nodiscard]] bool AbortMultipartUpload(
    std::string_view request_id,
    const std::string& upload_id,
    std::string& out_error) const;
```

`client/src/rpc/proxy_rpc.cpp` — 实现：
```cpp
bool ProxyRpc::AbortMultipartUpload(
    std::string_view request_id,
    const std::string& upload_id,
    std::string& out_error) const {
  if (!ok()) {
    out_error = std::string{"proxy channel not ready: "} + init_error();
    return false;
  }
  brpc::Controller controller;
  ApplyTimeout(controller);

  ::us3_turbo::proxy::AbortMultipartUploadRequest req;
  req.set_request_id(std::string(request_id));
  req.set_upload_id(upload_id);

  ::us3_turbo::proxy::AbortMultipartUploadResponse resp;
  stub()->AbortMultipartUpload(&controller, &req, &resp, nullptr);
  if (controller.Failed()) {
    out_error = controller.ErrorText();
    spdlog::error("AbortMultipartUpload (req={}): rpc failed: {}",
                  request_id, controller.ErrorText());
    return false;
  }
  if (!resp.ok()) {
    out_error = resp.error_message();
    return false;
  }
  return true;
}
```

`client/include/us3_turbo/client/client.h` — 声明：
```cpp
/** @brief 终止分段上传，释放 proxy 会话。 */
[[nodiscard]] bool AbortMultipartUpload(
    const std::string& upload_id,
    std::string& out_error) const;
```

`client/src/client.cpp` — 实现：
```cpp
bool Client::AbortMultipartUpload(
    const std::string& upload_id,
    std::string& out_error) const {
  if (!initialized_) {
    out_error = "Client not initialized";
    return false;
  }
  const std::string request_id = detail::MakeRequestId();
  return proxy_->AbortMultipartUpload(request_id, upload_id, out_error);
}
```

### L2 放宽 part 连续性为"升序不重复" — `session_manager.cpp:122-136`
```cpp
bool SessionManager::ValidatePartList(const UploadSession& session,
                                      std::string& error) {
  if (session.parts.empty()) {
    error = "no parts uploaded";
    return false;
  }
  // s3 语义：part_number 升序且无重复（允许间隙，如 1,3,5）。
  for (std::size_t i = 1; i < session.parts.size(); ++i) {
    if (session.parts[i].part_number <= session.parts[i - 1].part_number) {
      error = "part_number not strictly ascending at index " + std::to_string(i);
      return false;
    }
  }
  return true;
}
```
> parts 已在 CompleteSession 里按 part_number 排序（90-93），此处只校验严格升序。

### L3 统一 path 校验风格 — `proxy_control_plane_service.cpp`
v1 不支持 PATH_ALL，`GdsPut`/`UcxPut` 的 `HasPath` 位运算改为精确匹配，与 multipart 一致：
```cpp
// GdsPut（108）:
if (request->path() != ::us3_turbo::proxy::PATH_GDS) {
  cntl->SetFailed(PROXY_ERR_PATH_NOT_SUPPORTED, "GdsPut requires PATH_GDS");
  return;
}
// UcxPut（162）: 同理 PATH_UCX
```
删匿名 namespace 的 `HasPath`（18-22，改后无调用者）。

### L4 `AddPart` 覆盖告警 — `session_manager.cpp:60-68`
覆盖同 part_number 时若 part_size 变化，加 warn 日志（不阻断）：
```cpp
if (p != session->parts.end()) {
  if (p->part_size != part.part_size) {
    spdlog::warn("AddPart: part {} size changed {} -> {} (overwrite)",
                 part.part_number, p->part_size, part.part_size);
  }
  *p = part;
} else {
  session->parts.push_back(part);
}
```
> 需 `#include <spdlog/spdlog.h>`。

---

## 验证

### 编译 / 死代码
- [ ] 全量编译通过；无 `HexVal`/`ErrorMessage`/`gateway_id_`/`HasPath`/`backend_endpoint_` 残留引用
- [ ] `PROXY_ERR_UNSUPPORTED_PATH`/`NOT_IMPLEMENTED` 已删且无引用

### 生命周期（A1）
- [ ] 正常启停：proxy 收 SIGINT 后析构不 hang、不崩溃
- [ ] 清理线程能被析构立即唤醒 join（不等满 1 小时）

### 功能
- [ ] 单步 GDS/UCX、分段全流程回归通过（etag 与改前一致）
- [ ] client `Client::AbortMultipartUpload` 调用成功；之后再 `UploadPart`/`Complete` 该 upload_id → 返回 not found
- [ ] part 1,3,5（升序有间隙）Complete 成功；part 1,1 或 2,1 → 拒绝
- [ ] 多会话并发跑 multipart，block channel POOLED 无串行阻塞

### 链路隔离
- [ ] GdsPut/UcxPut、UploadPartGds/Ucx 改动各自独立，改一条不影响另一条编译/运行

---

## 变更文件清单

| 文件 | 涉及项 |
|---|---|
| `proxy/src/common/utils.cpp` | R1, R4, R5 |
| `proxy/src/common/errors.h` | R3 |
| `proxy/src/main.cpp` | R2, R7 |
| `proxy/src/service/proxy_control_plane_service.h` | R2, R6, A1, A2, A3 |
| `proxy/src/service/proxy_control_plane_service.cpp` | R2, R6, A1, A2, A3, L1, L3 |
| `proxy/src/multipart/session_manager.h` | L1 |
| `proxy/src/multipart/session_manager.cpp` | L1, L2, L4 |
| `proto/control_plane.proto` | A3 |
| `client/src/rpc/proxy_rpc.{h,cpp}` | A3 client 侧 |
| `client/include/us3_turbo/client/client.h` | A3 client 侧 |
| `client/src/client.cpp` | A3 client 侧 |
