# 提示词（改动2）：把 buffer 注册收进内部，调用方零感知

## 背景

上一轮重构后,GDS 调用方仍需手写:

```cpp
auto* gds = client.gds_channel();               // 暴露了内部句柄
if (gds == nullptr) { ... }
gds->RegisterDeviceBuffer(dev, bytes);          // 暴露了注册细节
...
gds->UnregisterDeviceBuffer(dev);
```

这违背「客户只关心链路模式」的目标,且两条链路**不对称**:
UCX 的 `AcquireDescriptor` 本就在 `PutOnce` 内部懒注册,调用方零感知;
GDS 的 `AcquireToken` **同样已经懒注册**(`gds_memory_manager.cpp`:未注册的 ptr
会 lazy register),所以调用方那几行注册代码**纯属多余**。

## 目标

调用方只需「选模式 + PutObject」,注册/注销全部内部化:

```cpp
Client client(std::move(opts));
if (!client.Initialize()) { ... }

ClientProxyPutRequest req;
req.bucket = "b"; req.key = "k"; req.object_size = bytes;
req.path = PutDataPath::kGds;          // ← 唯一要关心的:链路模式

ClientProxyPutResponse resp;
bool ok = client.PutObject(req, ConstBufferView{dev, bytes}, resp);
cudaFree(dev);                          // 无需先 Unregister
```

GDS 与 UCX 对称:两条链路都在各自 `PutOnce` 内部懒注册,对外不暴露任何注册 API。

## 改动清单

### 1. `Client`(client.h / client.cpp)

- **删除** `GdsPutChannel* gds_channel() const` 访问器(头 + 实现)。
- `Client` 公共面只剩:`Initialize` / `Shutdown` / `initialized` / `PutObject`。

### 2. `GdsPutChannel`(gds_put_channel.h / .cpp)

- 把 `RegisterDeviceBuffer` / `UnregisterDeviceBuffer` 从 **public 移除**
  (不再对外)。注册由 `PutOnce → AcquireToken` 内部懒注册完成,与 UCX 对称。
- 若要保留「首次 PUT 前预热注册」的能力,作为**内部私有**手段即可,不进公共 API。

### 3. 懒注册确认(gds_memory_manager.cpp)

- 确认 `AcquireToken` 对未注册 ptr 会自动 `DoRegister`(现状已如此,勿改逻辑)。
- 因此 `PutOnce` 不预先注册也能成功:首次 PUT 自动 pin,后续 PUT 命中注册表复用。

### 4. 注销/生命周期

- 注册表作为**进程级缓存**:buffer 被复用时只注册一次,直到 manager 析构
  统一释放(析构已有批量 unmap/PutDescriptor 逻辑)。
- 把析构里的 `"N buffer(s) not unregistered before shutdown"` 从 **warn 降为 debug**
  ——懒注册常驻是现在的**预期行为**,不再是"忘了注销"的告警。

### 5. 示例(examples/gds/*.cpp)

- 删除 `client.gds_channel()` / `RegisterDeviceBuffer` / `UnregisterDeviceBuffer`
  三处调用,改为「选 path + PutObject」直调(见上「目标」代码)。
- 删除示例里对 `gds_put_channel.h` 的 include(调用方不再需要)。

## 约束

- **PutObject 行为不变**:除去掉外部注册步骤,路由/重试/CRC/trace 全部保持。
- **两链路仍物理隔离**:改动不引入 gds↔ucx 交叉 include。
- **不改懒注册核心逻辑**:只是不再从外部暴露,`AcquireToken`/`AcquireDescriptor`
  内部注册流程原样保留。

## 已知限制(记 TODO,本轮不处理)

- 懒注册缓存以 `ptr` 为 key:若调用方 `cudaFree(dev)` 后同地址被重新
  `cudaMalloc` 成不同 buffer,注册表会命中旧项、复用陈旧 descriptor。
  当前项目 buffer 长期复用,风险低;后续可加「注册失效钩子 / (ptr,size,generation)
  校验」。UCX 路径同理。

## 验收

- [ ] 调用方代码里**不出现** `gds_channel` / `RegisterDeviceBuffer` /
      `UnregisterDeviceBuffer`。
- [ ] `Client` 公共 API 只有 `Initialize/Shutdown/initialized/PutObject`。
- [ ] 只设 `req.path = kGds` + `PutObject` 即可完成上传(首次自动懒注册)。
- [ ] GDS 与 UCX 调用方写法完全对称(都无需显式注册)。
- [ ] 示例编译不再 include `gds_put_channel.h`。
