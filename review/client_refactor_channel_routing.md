# Client 重构方案：链路模式路由（GDS / RDMA 各自成类）

> 只描述重构方案，**不改代码、不编译、不测试**。供后续实现参考。

## 1. 现状问题

`Client` 是一个大杂烩，把两条链路的细节都塞进了基类：

- 同时持有 `gds_mgr_` 和 `ucx_mgr_`，两条链路的生命周期耦合在一个类里。
- 两条链路的单次逻辑是 `client.cpp` 里的自由函数 `GdsPutOnce` / `UcxPutOnce`，
  不是独立类文件；且 `UcxPutOnce` 在文件里重复定义了两次（L192、L385）。
- 两份 CRC 校验（`VerifyGdsCrc32c` / `VerifyUcxCrc32c`）、两份 trace 分支都堆在一起。
- GDS 专属的 `RegisterDeviceBuffer` / `UnregisterDeviceBuffer` 暴露在通用 `Client` 上。
- 路由靠 `PutObject` 里的 `HasPath` if 链手写分发。

结果：客户要感知 GDS/UCX 细节，链路之间没有物理隔离（违背「GDS 后期难找机器回归、
两条链路要尽量独立」的约束）。

## 2. 目标

1. **客户只关心「链路模式」**：选 `kGds` / `kUcx`（未来 `kAll`），不关心内部走哪个方法。
2. **每条链路一个独立类文件**：GDS、RDMA 各自实现，互不引用、可单独编译/回归。
3. **`Client` 退化为薄路由层**：只做「模式 → 链路」分发 + 跨链路的公共逻辑（校验、重试）。

## 3. 目标结构

```
                 ┌──────────────┐
   客户 ──选模式──>│    Client    │  薄路由：校验 + 重试 + 按 mode 分发
                 └──────┬───────┘
                        │ 持有 PutChannel*（按 mode）
          ┌─────────────┴─────────────┐
          ▼                           ▼
  ┌───────────────┐           ┌───────────────┐
  │ GdsPutChannel │           │ UcxPutChannel │   ← 各自独立类文件
  │  gds_mgr      │           │  ucx_mgr      │      互不引用
  │  GDS CRC/trace│           │  UCX CRC/trace│
  └───────────────┘           └───────────────┘
          └───────────┬───────────────┘
                      ▼ 共享同一条 brpc channel
                 ┌──────────┐
                 │ ProxyRpc │  (GdsPut / UcxPut 走同一 endpoint)
                 └──────────┘
```

### 3.1 新增：链路基类 `PutChannel`

`client/src/transport/put_channel.h` —— 只定义「一条链路能做什么」的最小接口，
是唯一被 `Client` 依赖的抽象，不含任何 GDS/UCX 细节：

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

> 注意：这是**唯一**的共享抽象，仅用于路由。两条链路的内存管理器
> （`GdsMemoryManager` / `UcxMemoryManager`）保持现状，**不共享基类**，符合
> 「两条链路尽量独立」的约束。

### 3.2 新增：GDS 链路 `GdsPutChannel`

`client/src/transport/gds_put_channel.h/.cpp` —— 从 `client.cpp` 迁入并封装：

- 构造持有：`const ClientOptions&`、`const ProxyRpc&`、`GdsMemoryManager*`。
- `PutOnce()`：搬运原 `GdsPutOnce` 全部逻辑（`AcquireToken` → `GdsDataSource` →
  `proxy.GdsPut` → 结果 → 可选 `VerifyGdsCrc32c`（含 D2H）→ 可选 GDS trace）。
- GDS 专属的 device buffer 注册/注销也归到这里（见 §4）。

### 3.3 新增：RDMA 链路 `UcxPutChannel`

`client/src/transport/ucx_put_channel.h/.cpp` —— 与 GDS 平行、独立：

- 构造持有：`const ClientOptions&`、`const ProxyRpc&`、`UcxMemoryManager*`。
- `PutOnce()`：搬运原 `UcxPutOnce`（`AcquireDescriptor` → `UcxDataSource` →
  `proxy.UcxPut` → 结果 → 可选 `VerifyUcxCrc32c`（host 直算）→ 可选 UCX trace）。
- 顺手删掉 `client.cpp` 里重复的第二份 `UcxPutOnce`。

### 3.4 公共小工具下沉

`MakeRequestId` 与 `TraceLatency`（含 `LatencyStage`）是链路无关的通用设施，
下沉到 `client/src/transport/put_trace.h`（或 `common/`），两条链路都 include。
**只有这两个通用工具共享**；link 特有的 CRC / 描述符逻辑各留各的，不复用。

### 3.5 `Client` 瘦身为路由层

`Client` 只保留：

- `options_`、`proxy_`（单条 brpc channel，两链路共用）。
- 两个链路成员：`std::unique_ptr<PutChannel> gds_channel_ / ucx_channel_`
  （`Initialize` 时按可用性构造；UCX manager 不可用则该指针为空）。
- `ExecutePutWithRetry` 重试模板（跨链路公共，保留在此）。
- `ValidatePutPath` + 大小上限校验（公共，保留在此）。

`PutObject` 变成纯分发：

```cpp
bool Client::PutObject(const ClientProxyPutRequest& req,
                       ConstBufferView buf, ClientProxyPutResponse& resp) const {
  // 1. initialized / path 校验 / 大小上限（保持现状）
  // 2. 按 mode 取链路
  PutChannel* ch = SelectChannel(req.path);      // kGds→gds_channel_, kUcx→ucx_channel_
  if (ch == nullptr) return false;               // 链路不可用/未指定
  // 3. 统一重试 + 回填对应 result（gds_result / ucx_result）
  return ExecutePutWithRetry(req, "PutObject", [&]{
    PutPathResult r;
    bool ok = ch->PutOnce(req, buf, r);
    resp.SetResultFor(req.path, r);              // 按 path 写 gds_result / ucx_result
    return ok;
  });
}
```

> `SelectChannel` 就是模式路由的唯一落点，取代原来散落的 `HasPath` if 链。
> `kAll` 未来在此扩展为「依次驱动两条链路」，路由点集中、改动可控。

## 4. GDS 专属 API 的归属

`RegisterDeviceBuffer` / `UnregisterDeviceBuffer` 是 GDS 独有的。两种处理，二选一：

- **方案 A（推荐）**：移到 `GdsPutChannel`，通用 `Client` 不再暴露；需要注册的
  用户通过 GDS 链路句柄操作。最彻底地把 GDS 细节挡在基类外。
- **方案 B（兼容优先）**：`Client` 保留这两个薄转发方法，内部转调 `gds_channel_`。
  公共 API 不变，代价是基类仍留一点 GDS 痕迹。

## 5. 文件清单

| 动作 | 文件 |
|------|------|
| 新增 | `client/src/transport/put_channel.h`（基类接口）|
| 新增 | `client/src/transport/gds_put_channel.h/.cpp` |
| 新增 | `client/src/transport/ucx_put_channel.h/.cpp` |
| 新增 | `client/src/transport/put_trace.h`（`MakeRequestId` + `TraceLatency` 下沉）|
| 改   | `client/src/client.cpp`（删两个 `*PutOnce` 及重复定义、CRC、trace；改为路由）|
| 改   | `client/include/us3_turbo/client/client.h`（成员改为 `PutChannel` 指针；GDS API 按 §4 处理）|

## 6. 约束

1. **两条链路物理隔离**：`gds_put_channel.*` 与 `ucx_put_channel.*` 互不 include，
   各自只依赖自己的 manager；唯一共享的是 `PutChannel` 抽象与 `put_trace.h` 通用工具。
2. **对外行为不变**：`PutObject` 入参/出参、日志格式、重试语义、CRC/trace 行为
   与现状逐字一致（纯搬运，不改逻辑）。
3. **重试留在 `Client`**：链路类只实现「单次尝试」，重试/deadline 由基类统一控制。
4. **`ProxyRpc` 单实例共享**：不为每条链路各建 channel，`Client` 持有、按引用传入。

## 7. 验收（行为层面，不含编译/测试）

- 客户代码只需 `req.path = kGds | kUcx` 即可选链路，无需感知内部方法名。
- `client.cpp` 不再有 `GdsPutOnce` / `UcxPutOnce` 自由函数，重复定义消除。
- 新增/删除一条链路只动对应 `*_put_channel.*` + `SelectChannel` 一处，其余不改。
