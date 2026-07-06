# Proxy 端代码 Review — 架构 / 逻辑 / 功能 / 既往要求

范围：`proxy/src/` 全部（main、common/errors、common/utils、multipart/*、service/*）。
每项标 **严重度**（🔴改 / 🟡建议 / 🟢可选）。

---

## 一、架构

### A1 🔴 TTL 清理线程 `detach()` 存在生命周期隐患
`proxy_control_plane_service.cpp:65-76`：清理线程 `detach()`，析构只 `stop_cleanup_ = true` 不 join。

问题：
- 析构后 detached 线程可能仍在 `sleep_for(1h)` 或正执行 `CleanupExpiredSessions`，
  此时 `session_manager_` 已析构 → **访问已销毁成员**（UB）。
- 停止标志检查在 1 小时 sleep 之后，`stop_cleanup_` 实际最长 1 小时才生效。

当前实践上安全（service 在 main 栈上、进程退出时才析构，server 已 Join），但属设计隐患。

**改法**：
```cpp
// 成员：std::thread cleanup_thread_;（不 detach）
// 析构：
stop_cleanup_.store(true);
cleanup_cv_.notify_all();          // 用 condition_variable 替代 sleep_for
if (cleanup_thread_.joinable()) cleanup_thread_.join();
```
把 `sleep_for(1h)` 换成 `cv.wait_for(lock, 1h, [&]{ return stop_cleanup_; })`，
既能被析构立即唤醒、又能 join，消除 UB。

### A2 🟡 backend channel 用 `CONNECTION_TYPE_SINGLE`，多会话并发下成瓶颈
`proxy_control_plane_service.cpp:47`：control_stub 与 block_stub **共用一条 single 连接**。
block 调用已串行化（每 part 内），若多个 client 并发跑 multipart，所有 PutBlock
挤在一条连接上排队。

**改法**：block 级 channel 用独立 channel + `CONNECTION_TYPE_POOLED`（或 `SHORT`），
与单步转发 channel 分开。单步路径可保持 single。

### A3 🟡 会话无显式 Abort，仅靠 3 天 TTL 回收
client 中途放弃上传的 session 要等 3 天 TTL 才清。s3 有 `AbortMultipartUpload`。
若走 s3proxy 兼容，缺这个 API。

**改法**：加 `AbortMultipartUpload` RPC（proto + handler），调 `CleanupSession`。v1 可推迟，但先记。

---

## 二、逻辑 / 正确性

### L1 🟡 `GetSession` 裸指针在锁外被使用
`proxy_control_plane_service.cpp:239,246`：`GetSession` 返回后 shared_lock 已释放，
随后 `session->path`（246）在锁外读。理论上并发 `CleanupSession`（同 upload_id 的
Complete 或 TTL）可 erase 该 session → 悬垂。

实践低危（client 不会边传 part 边 Complete），但是真实 sharp edge。

**改法（二选一）**：
- `GetSession` 直接返回需要的值（如 `std::optional<PutDataPath> GetSessionPath(upload_id)`），锁内读完即返回；或
- 让 `AddPart` 之外的读也走"锁内取值"模式，不暴露裸指针。

### L2 🟡 `ValidatePartList` 强制连续 1..N，比 s3 严格
`session_manager.cpp:122-136`：要求 part_number 严格连续。s3 允许升序但有间隙
（如 1,3,5）。若 s3proxy 兼容是目标，这里更严。

**改法**：改为"升序且无重复"即可，不强制连续。或明确文档标注 Us3Turbo 只支持连续 part。

### L3 🟢 `CreateMultipartUpload` 用精确匹配、单步用 `HasPath` 位运算
`CreateMultipartUpload`（212-213）用 `path != PATH_GDS && path != PATH_UCX`，
而 `GdsPut`（108）用 `HasPath` 位运算。两处语义不同但各自正确。可统一风格（都用精确匹配，
因 v1 不支持 PATH_ALL）。

### L4 🟢 `AddPart` 覆盖同 part_number 时不校验 part_size 一致
重传同 part 若 size 变了会静默覆盖。低危（客户端一般不变），可加日志告警。

---

## 三、功能

### F1 🟡 单步 16MB 上限校验位置
`GdsPut`/`UcxPut` 已加 `object_size > kMaxUploadBytes` 校验（100、159），正确。
但 `object_size` 是 client 声明值，未与实际传输字节比对（backend 侧 length 才是真实）。
v1 可接受，记一笔。

### F2 🟢 `CompleteMultipartUpload` 失败不清理会话（有意）
校验失败保留 session 供重试，成功才 `CleanupSession`（393）。逻辑正确。
但若客户端反复 Complete 失败，session 直到 TTL 才回收——配合 A3 的 Abort 更完整。

---

## 四、既往要求（20 条规范）

### R1 🔴 死代码：`utils.cpp:15 HexVal` 从未被使用 — req #4/#5
`HexVal` 定义在匿名 namespace，全项目无调用（GenUuid 用 snprintf 生成 hex，不解析）。
**删除**。

### R2 🔴 死代码：`gateway_id_` 成员只存不用 — req #4
`proxy_control_plane_service`：构造存入 `gateway_id_`（35），除头注释外无任何使用。
**删除成员 + 构造参数**（或如需保留标识用途，至少加日志用上；否则删）。

### R3 🟡 死代码：`errors.h` 的 `ErrorMessage()` + 两个错误码未使用 — req #4/#6
- `ErrorMessage()`（25-36）：全项目无调用（注释自己写"可选"）。
- `PROXY_ERR_UNSUPPORTED_PATH`(13001)、`PROXY_ERR_NOT_IMPLEMENTED`(90001)：
  仅在 `ErrorMessage()` 内部出现，无任何 `SetFailed` 使用。

按 req #6"用到再加"，**删 `ErrorMessage()` 与两个未用错误码**；保留实际使用的 5 个码。

### R4 🟡 `GenUuid` 种子 `(random_device{}() << 1)` 左移丢随机性 — req 之前 D2
`utils.cpp:31`：左移 1 位丢掉最高位随机性，无意义。
**改法**：`std::random_device{}()` 直接用，或 `seed_seq{rd(), rd(), rd(), rd()}`。

### R5 🟡 `GenUuid` 内注释偏多 — req #1（≤1 行/块）
`utils.cpp:28-29、36` 多行注释可压到 ≤1 行。

### R6 🟢 `backend_endpoint_` 仅构造期使用，可降为局部 — req #4（弱）
只在构造函数里读（36-61），非成员必要。但头注释引用了它做线程安全说明，
降为局部需同步改注释。收益小，可选。

### R7 🟢 main.cpp flag 注释轻微过时
`main.cpp:16`：`--backend_endpoint` 注释写"proxy forwards GdsPut here"，
实际也用于 UcxPut 与 block PutBlock。改为"backend data plane endpoint"。

---

## 汇总（按处理顺序）

| # | 位置 | 严重度 | 类型 |
|---|---|---|---|
| A1 | 清理线程 detach | 🔴 | 生命周期 UB 隐患 |
| R1 | utils HexVal | 🔴 | 死代码 |
| R2 | gateway_id_ | 🔴 | 死成员 |
| A2 | single 连接 | 🟡 | 并发瓶颈 |
| A3 | 无 Abort API | 🟡 | s3 兼容缺口 |
| L1 | GetSession 裸指针 | 🟡 | 锁外访问 |
| L2 | 强制连续 part | 🟡 | s3 兼容 |
| R3 | ErrorMessage + 2 码 | 🟡 | 死代码 |
| R4 | UUID 种子 | 🟡 | 写法可疑 |
| R5 | GenUuid 注释 | 🟡 | 注释过多 |
| L3/L4/F1/F2/R6/R7 | — | 🟢 | 可选 |

---

## 优先建议

**立即修（低风险、纯清理）**：R1（HexVal）、R2（gateway_id_）、R3（ErrorMessage+死码）、R4/R5（UUID）、R7（注释）。

**建议修（有实际隐患）**：
- **A1** 清理线程改 join + condition_variable —— 唯一的真实 UB 隐患，虽实践安全但值得根治。
- **L1** GetSession 锁外裸指针 —— 改成锁内取值，消除悬垂可能。

**看需求定（s3proxy 兼容 / 性能）**：
- **A2** block channel 独立 + POOLED（多会话并发性能）
- **A3** AbortMultipartUpload（s3 API 完整性）
- **L2** part 连续性放宽到升序（s3 兼容）

需我按哪些项生成修改提示词？我建议至少 A1 + R1/R2/R3/R4 一起做（清理 + 根治 UB 隐患）。
