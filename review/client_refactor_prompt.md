# 提示词：Client 链路路由重构 + 内存管理器共享基类

## 角色与背景

你是一名 C++ 工程师，负责重构一个**新建**对象存储项目的 client 模块。
这是新工程，**不涉及运维、兼容性，无需运行编译/测试**，可自由修改与删除代码。
目标语言 C++17，现有依赖：brpc、protobuf、spdlog、cuObj(GDS)、UCX。

## 总目标

1. **客户只关心「链路模式」**：调用方设 `req.path = kGds / kUcx`（未来 `kAll`）即可，
   不感知内部走 `GdsPut` 还是 `UcxPut`。
2. **GDS 与 RDMA 各自独立成类文件**：两条链路互不引用，可单独回归（GDS 机器稀缺）。
3. **`Client` 瘦身为薄路由层**：只做「模式 → 链路」分发 + 公共逻辑（校验、重试）。
4. **两个内存管理器共享一个基类骨架**：注册表/锁/幂等注册流程复用；但 pin/map、
   token/描述符生成等**细节各自实现**。

## 硬约束（务必遵守）

- **物理隔离**：`gds_put_channel.*` 与 `ucx_put_channel.*` 互不 include；
  `gds_memory_manager.*` 与 `ucx_memory_manager.*` 互不 include。
- **共享头保持“干净”**：链路基类、内存管理器基类、通用工具这几个共享头
  **不得引入 cuObj / ucp 头**（否则 GDS 单独回归会被 UCX 依赖污染，反之亦然）。
- **行为逐字不变**：`PutObject` 入参/出参、日志文本与格式、重试/deadline 语义、
  CRC 校验行为、latency trace 输出，全部与现状一致——本次是**纯搬运 + 拆类**，不改逻辑。
- 不新增第三方依赖。

---

## 阶段 1：链路抽象基类 + 两条链路成类

### 1.1 新增 `client/src/transport/put_channel.h`

`Client` 唯一依赖的链路抽象，最小接口，不含任何 GDS/UCX 细节：

```cpp
class PutChannel {
 public:
  virtual ~PutChannel() = default;
  // 单次 PUT 尝试（不含重试）：内部完成 描述符获取 → proxy RPC → CRC → trace。
  [[nodiscard]] virtual bool PutOnce(const ClientProxyPutRequest& request,
                                     ConstBufferView buffer,
                                     PutPathResult& result) const = 0;
};
```

### 1.2 新增 `client/src/transport/put_trace.h`（通用工具下沉）

把 `client.cpp` 里链路无关的 `MakeRequestId()` 与 `TraceLatency()`（含 `LatencyStage`）
原样移到这里，两条链路都 include。**只有这两个通用工具共享**，link 特有逻辑不下沉。

### 1.3 新增 `client/src/transport/gds_put_channel.h/.cpp`

- 构造持有：`const ClientOptions&`、`const ProxyRpc&`、`GdsMemoryManager*`。
- `PutOnce()`：搬运原自由函数 `GdsPutOnce` 的完整逻辑
  （`AcquireToken` → 构 `GdsDataSource` → `proxy.GdsPut` → 回填 result →
  可选 `VerifyGdsCrc32c`（含 D2H 拷贝）→ 可选 GDS trace）。
- `VerifyGdsCrc32c` 作为本文件的私有实现（GDS 专属，不与 UCX 复用）。

### 1.4 新增 `client/src/transport/ucx_put_channel.h/.cpp`

- 构造持有：`const ClientOptions&`、`const ProxyRpc&`、`UcxMemoryManager*`。
- `PutOnce()`：搬运原 `UcxPutOnce`
  （`AcquireDescriptor` → 构 `UcxDataSource` → `proxy.UcxPut` → 回填 result →
  可选 `VerifyUcxCrc32c`（host 直算）→ 可选 UCX trace）。
- `VerifyUcxCrc32c` 作为本文件私有实现。
- **删除 `client.cpp` 里重复定义的第二份 `UcxPutOnce`**（当前 L192 与 L385 重复）。

---

## 阶段 2：内存管理器共享基类（骨架共享、细节各自实现）

### 2.1 新增 `client/src/transport/buffer_registry.h`

抽取两个 manager 都有的公共骨架——注册表 + 串行化锁 + **幂等注册/注销流程**。
用模板容纳不同的句柄类型（GDS 存 `size_t`，UCX 存 `ucp_mem_h`），
真正的 pin/map 与释放由派生类通过纯虚钩子实现。**此头不含 cuObj/ucp 依赖**：

```cpp
template <typename Handle>
class BufferRegistry {
 public:
  virtual ~BufferRegistry() = default;

 protected:
  // 幂等注册：已在表内则直接成功；否则调派生类 DoRegister 真正 pin/map 后入表。
  [[nodiscard]] bool RegisterBuffer(void* ptr, std::size_t size) {
    std::lock_guard<std::mutex> lk(mu_);
    if (registered_.count(ptr)) return true;
    Handle h{};
    if (!DoRegister(ptr, size, h)) return false;
    registered_.emplace(ptr, std::move(h));
    return true;
  }

  [[nodiscard]] bool UnregisterBuffer(void* ptr) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = registered_.find(ptr);
    if (it == registered_.end()) return true;   // 幂等
    DoUnregister(ptr, it->second);
    registered_.erase(it);
    return true;
  }

  // —— 细节各自实现（cuObj / ucp 分别在派生类里）——
  [[nodiscard]] virtual bool DoRegister(void* ptr, std::size_t size, Handle& out) = 0;
  virtual void DoUnregister(void* ptr, Handle& handle) = 0;

  std::mutex                          mu_;         // 串行化注册表 + 单 worker 访问
  std::unordered_map<void*, Handle>   registered_;
};
```

### 2.2 改 `client/src/gds_transport/gds_memory_manager.h/.cpp`

- `class GdsMemoryManager : public BufferRegistry<std::size_t>`。
- 删除自有的 `registration_mu_` / `registered_` / `RegisterBufferUnderLock`，
  改用基类的 `mu_` / `registered_` / `RegisterBuffer` / `UnregisterBuffer`。
- 实现 `DoRegister`（把原 `RegisterBufferUnderLock` 里真正 pin 进 BAR1 的 cuObj
  逻辑搬进来，`out` 存该 buffer 的 size）与 `DoUnregister`（对应释放）。
- 保留 GDS 专属、**不进基类**的 `AcquireToken(ptr,size,offset,Token&)` 与 `Token` RAII。
- 公共 `RegisterBuffer/UnregisterBuffer` 现由基类提供（签名不变，行为不变）。

### 2.3 改 `client/src/rdma_transport/ucx_memory_manager.h/.cpp`

- `class UcxMemoryManager : public BufferRegistry<ucp_mem_h>`。
- 删除自有的 `mu_` / `registered_` / `RegisterBufferUnderLock`，改用基类同名设施。
- 实现 `DoRegister`（原 `RegisterBufferUnderLock` 里的 `ucp_mem_map`，`out` 存 `ucp_mem_h`）
  与 `DoUnregister`（`ucp_mem_unmap`）。
- 保留 UCX 专属、**不进基类**的 `AcquireDescriptor` 与 `Descriptor`
  （内部懒注册改为调基类 `RegisterBuffer`）。

> 注意：基类是模板，`GdsMemoryManager` 只实例化 `BufferRegistry<size_t>`、
> `UcxMemoryManager` 只实例化 `BufferRegistry<ucp_mem_h>`，两者仍不互相依赖，
> GDS 回归不会被 UCX 拖累。

---

## 阶段 3：`Client` 瘦身为路由层

改 `client/include/us3_turbo/client/client.h` 与 `client/src/client.cpp`：

- 成员改为：`options_`、`std::unique_ptr<ProxyRpc> proxy_`（两链路共用的单 channel）、
  `std::unique_ptr<PutChannel> gds_channel_`、`std::unique_ptr<PutChannel> ucx_channel_`。
  删除 `gds_mgr_` / `ucx_mgr_` 直接成员（交由各 channel 持有）。
- `Initialize()`：建 `proxy_`；取 `GdsMemoryManager` 实例并构 `gds_channel_`；
  取 `UcxMemoryManager` 实例并构 `ucx_channel_`（UCX 不可用则该指针留空 + warn，
  与现状一致）。
- 保留公共逻辑：`ExecutePutWithRetry` 重试模板、`ValidatePutPath`、大小上限校验。
- 删除 `client.cpp` 里的 `GdsPutOnce` / `UcxPutOnce`（已迁入 channel）、
  两份 `Verify*Crc32c`、trace 分支。
- `PutObject` 变为纯分发：

```cpp
bool Client::PutObject(const ClientProxyPutRequest& req, ConstBufferView buf,
                       ClientProxyPutResponse& resp) const {
  if (!initialized_) { /* 原错误日志 */ return false; }
  if (!ValidatePutPath(req)) return false;
  /* 原 put_single_max_bytes 大小上限校验，保持不变 */

  PutChannel* ch = SelectChannel(req.path);   // kGds→gds_channel_ ; kUcx→ucx_channel_
  if (ch == nullptr) { /* 原“manager 未初始化”日志 */ return false; }

  return ExecutePutWithRetry(req, "PutObject", [&]() -> bool {
    PutPathResult r;
    const bool ok = ch->PutOnce(req, buf, r);
    if (HasPath(req.path, PutDataPath::kGds)) resp.gds_result = r;
    else                                      resp.ucx_result = r;
    return ok;
  });
}
```

- 新增私有 `SelectChannel(PutDataPath)`：`kGds→gds_channel_.get()`、
  `kUcx→ucx_channel_.get()`、其余返回 `nullptr`。**这是模式路由的唯一落点**，
  取代原 `HasPath` if 链；`kAll` 未来只在这一处扩展为“依次驱动两条链路”。

---

## 阶段 4：GDS 专属注册 API 归属（方案 A）

- 把 `RegisterDeviceBuffer` / `UnregisterDeviceBuffer` 从通用 `Client` 移出，
  作为 `GdsPutChannel` 的公共方法（内部转调其持有的 `GdsMemoryManager`）。
- 通用 `Client` 不再暴露这两个 GDS 专属方法，基类彻底不含 GDS 痕迹。
- 需要注册 device buffer 的调用方通过 GDS 链路句柄操作。

---

## 文件清单

| 动作 | 文件 |
|------|------|
| 新增 | `client/src/transport/put_channel.h` |
| 新增 | `client/src/transport/put_trace.h` |
| 新增 | `client/src/transport/buffer_registry.h` |
| 新增 | `client/src/transport/gds_put_channel.h/.cpp` |
| 新增 | `client/src/transport/ucx_put_channel.h/.cpp` |
| 改   | `client/src/gds_transport/gds_memory_manager.h/.cpp`（继承基类）|
| 改   | `client/src/rdma_transport/ucx_memory_manager.h/.cpp`（继承基类）|
| 改   | `client/include/us3_turbo/client/client.h`（成员改为 channel 指针）|
| 改   | `client/src/client.cpp`（删 `*PutOnce`/CRC/trace，改为 `SelectChannel` 路由）|

## 迁移映射表（从 → 到，纯搬运）

| 现状位置 | 迁移到 |
|----------|--------|
| `client.cpp` `GdsPutOnce` | `gds_put_channel.cpp` `GdsPutChannel::PutOnce` |
| `client.cpp` `UcxPutOnce`（去重）| `ucx_put_channel.cpp` `UcxPutChannel::PutOnce` |
| `client.cpp` `VerifyGdsCrc32c` | `gds_put_channel.cpp` 私有 |
| `client.cpp` `VerifyUcxCrc32c` | `ucx_put_channel.cpp` 私有 |
| `client.cpp` `MakeRequestId`/`TraceLatency`/`LatencyStage` | `put_trace.h` |
| `gds/ucx_memory_manager` 的 `mu_`+`registered_`+`RegisterBufferUnderLock` | `buffer_registry.h` 基类 |
| `Client::RegisterDeviceBuffer`/`UnregisterDeviceBuffer` | `GdsPutChannel` 公共方法 |

## 验收清单（行为层面，不含编译/测试）

- [ ] 调用方仅凭 `req.path` 选链路，不出现 `GdsPut`/`UcxPut` 等内部名。
- [ ] `client.cpp` 内不再有 `GdsPutOnce`/`UcxPutOnce`/`Verify*Crc32c`，重复定义消除。
- [ ] `gds_put_channel.*` 不 include 任何 UCX 头；`ucx_put_channel.*` 不 include 任何 GDS 头。
- [ ] `buffer_registry.h`/`put_channel.h`/`put_trace.h` 不 include cuObj/ucp。
- [ ] 两个 memory manager 均继承 `BufferRegistry<Handle>`，注册表/锁/幂等流程走基类，
      pin/map（`DoRegister`/`DoUnregister`）各自实现。
- [ ] `Client` 成员只剩 `options_`/`proxy_`/两个 `PutChannel` 指针 + 公共校验/重试。
- [ ] 新增/删除一条链路只动对应 `*_put_channel.*` + `SelectChannel` 一处。
- [ ] 所有日志文本、CRC、trace 输出与重构前逐字一致。
