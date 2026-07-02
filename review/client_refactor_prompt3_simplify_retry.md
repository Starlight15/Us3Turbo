# 提示词（改动3）：简化重试逻辑为 retry-once

## 角色与背景

你是一名 C++ 工程师，负责继续重构对象存储项目的 client 模块。
这是新工程，**不涉及运维、兼容性，无需运行编译/测试**，可自由修改与删除代码。
目标语言 C++17，现有依赖：brpc、protobuf、spdlog、cuObj(GDS)、UCX。

## 改动背景

当前 `PutObject` 实现有三层嵌套 lambda：

```cpp
PutChannel* ch = SelectChannel(request.path);
return ExecutePutWithRetry(request, "PutObject", [&]() -> bool {
  PutPathResult r;
  const bool ok = ch->PutOnce(request, buffer, r);
  if (HasPath(request.path, PutDataPath::kGds)) response.gds_result = r;
  else                                          response.ucx_result = r;
  return ok;
});

// ExecutePutWithRetry 又嵌套 ExecuteWithRetry(RetryPolicy{}, [&]() { ... })
```

过度抽象，引入 `RetryPolicy` / `ExecuteWithRetry` / `ExecutePutWithRetry` 三层包装，
但实际需求只是 **retry-once**（失败后再试一次）。

## 总目标

简化为直接两次调用，无 lambda、无模板、无复杂嵌套：

```cpp
bool Client::PutObject(...) const {
  // 校验 + 路由
  PutChannel* ch = SelectChannel(request.path);
  if (ch == nullptr) { ... }

  PutPathResult result;
  
  // 第一次尝试
  if (!ch->PutOnce(request, buffer, result)) {
    // 失败，等待后重试一次
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ch->PutOnce(request, buffer, result);  // 第二次（成功/失败都接受）
  }

  // 回填结果
  if (request.path == PutDataPath::kGds) response.gds_result = result;
  else                                    response.ucx_result = result;
  
  return result.ok;
}
```

---

## 改动清单

### 1. `client.cpp`

#### 删除

- `ExecutePutWithRetry` 模板方法（L83-96，包括定义和实例化）
- `#include "client/src/common/retry_policy.h"`（L9）

#### 改写 `PutObject`（L123-158）

**新实现**（完整替换）：

```cpp
bool Client::PutObject(const ClientProxyPutRequest& request,
                       ConstBufferView buffer,
                       ClientProxyPutResponse& response) const {
  if (!initialized_) {
    spdlog::error("PutObject: {} (req={})", kNotInitializedMsg, request.request_id);
    return false;
  }
  if (!ValidatePutPath(request)) {
    return false;
  }

  // 大小上限校验（沿用原 put_single_max_bytes）
  const auto max_put = options_.put_single_max_bytes;
  if (max_put != 0 && buffer.size > max_put) {
    spdlog::warn("PutObject: bucket={}/{} body size {} exceeds put_single_max_bytes {}; "
                 "use multipart upload",
                 request.bucket, request.key, buffer.size, max_put);
    return false;
  }

  PutChannel* ch = SelectChannel(request.path);
  if (ch == nullptr) {
    spdlog::error("PutObject: {} channel not initialized (req={})",
                  request.path == PutDataPath::kGds ? "GDS" : "UCX",
                  request.request_id);
    return false;
  }

  PutPathResult result;
  
  // 第一次尝试
  if (!ch->PutOnce(request, buffer, result)) {
    // 失败，等待 100ms 后重试一次
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ch->PutOnce(request, buffer, result);  // 第二次（成功/失败都接受）
  }

  // 回填结果到对应路径
  if (request.path == PutDataPath::kGds) {
    response.gds_result = result;
  } else {
    response.ucx_result = result;
  }
  
  return result.ok;
}
```

**改动点**：
1. 删除 `ExecutePutWithRetry` 调用，展开为两次 `PutOnce`。
2. 删除 `deadline` 检查（retry-once 不需要复杂超时）。
3. 用 `request.path == PutDataPath::kGds` 直接判等（删除 `HasPath` 位运算）。
4. `result` 在外层定义一次，两次 `PutOnce` 都更新同一个对象。
5. 固定退避 100ms（简单场景够用，不引入 `RetryPolicy` 复杂度）。

### 2. `client.h`

#### 删除

- `ExecutePutWithRetry` 模板方法声明（L88-91）。

**保留**：
- `ValidatePutPath`
- `SelectChannel`

---

## 硬约束（务必遵守）

1. **行为简化但保持正确**：retry-once（失败后再试一次）是明确需求，无需更复杂重试策略。
2. **日志文本保持**：`kNotInitializedMsg` / "channel not initialized" / "exceeds put_single_max_bytes" 
   等日志与原实现一致。
3. **路由/校验逻辑不变**：`ValidatePutPath` / `SelectChannel` / 大小上限校验保持原样。
4. **两链路仍物理隔离**：不引入 gds↔ucx 交叉 include。

---

## 删除的依赖

- `client/src/common/retry_policy.h`（整个文件可删，若无其他引用）

**注意**：若 `retry_policy.h` / `RetryPolicy` 在其他地方（如 `proxy` / `backend`）仍被使用，
保留文件；只删 `client.cpp` 的 include。

---

## 验收清单（行为层面，不含编译/测试）

- [ ] `client.cpp` 不再有 `ExecutePutWithRetry` / `ExecuteWithRetry` / `RetryPolicy`。
- [ ] `PutObject` 实现清晰：校验 → 路由 → 两次 `PutOnce` → 回填结果。
- [ ] 无 lambda、无模板嵌套、无 `deadline` 超时检查。
- [ ] 重试策略明确：失败后等 100ms 再试一次，总共最多两次调用。
- [ ] 日志文本与原实现一致（除去掉 "retry deadline exceeded" 日志）。
- [ ] `result.ok` 决定最终返回值（最后一次 `PutOnce` 的结果）。

---

## 文件改动汇总

| 文件 | 改动 |
|------|------|
| `client/include/us3_turbo/client/client.h` | 删除 `ExecutePutWithRetry` 模板声明 |
| `client/src/client.cpp` | 删除 `ExecutePutWithRetry` 实现、删除 `retry_policy.h` include、简化 `PutObject` 为两次 `PutOnce` |
| `client/src/common/retry_policy.h` | （可选）若无其他引用可整个删除 |

---

## 代码对比（简化前 vs 简化后）

### 简化前（三层嵌套）

```cpp
return ExecutePutWithRetry(request, "PutObject", [&]() -> bool {
  PutPathResult r;
  const bool ok = ch->PutOnce(request, buffer, r);
  if (HasPath(request.path, PutDataPath::kGds)) response.gds_result = r;
  else                                          response.ucx_result = r;
  return ok;
});

template <typename PutFunc>
bool Client::ExecutePutWithRetry(...) const {
  const auto deadline = ...;
  return ExecuteWithRetry(RetryPolicy{}, [&]() -> bool {
    if (std::chrono::steady_clock::now() >= deadline) { ... }
    return put_operation();
  });
}
```

### 简化后（直接两次调用）

```cpp
PutPathResult result;

if (!ch->PutOnce(request, buffer, result)) {
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ch->PutOnce(request, buffer, result);
}

if (request.path == PutDataPath::kGds) response.gds_result = result;
else                                    response.ucx_result = result;

return result.ok;
```

---

**最终目标**：`PutObject` 实现清晰易读、无过度抽象、retry-once 语义明确，
符合「从最优化角度设计、不炫技、不嵌套复杂」的原则。
