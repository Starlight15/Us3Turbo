# Proxy ↔ Backend 交互结构 - 实施总览

## 📋 实施阶段概览

本文档描述了 Proxy 和 Backend 交互结构的完整实施方案，分为 6 个可增量实现的阶段。

---

## 🎯 整体目标

实现 Client → Proxy → Backend 的三层架构：
- **Client**：发起对象上传请求（GDS 或 UCX 路径）
- **Proxy**：接收请求，拆分为多个 block，转发给 Backend
- **Backend**：主动从 Client 拉取数据（Pull 模式），写入存储系统

---

## 📐 架构设计

### 数据流向

```
┌─────────┐         ┌─────────┐         ┌──────────┐
│ Client  │────────>│  Proxy  │────────>│ Backend  │
│         │ PUT Req │         │ PutBlock│          │
│ (GPU/   │         │ (Split) │         │ (Pull &  │
│  Host)  │<────────│         │<────────│  Write)  │
└─────────┘  Result └─────────┘  Result └──────────┘
     │                                        │
     └────────────────────────────────────────┘
              Backend 主动拉取数据 (RDMA)
```

### 关键特性

1. **Pull 模式**：Backend 主动拉取，避免 Client 推送（减少 Client 端逻辑）
2. **分块上传**：Proxy 将大对象拆分为 5MB 块，并发上传
3. **路径隔离**：GDS 和 UCX 完全独立（代码、RPC、测试）
4. **端到端校验**：Client 计算 CRC → Backend 验证 → Proxy 汇总

---

## 🗂️ 阶段划分

### ✅ 阶段1：定义数据结构
**文件**：`proxy_backend_phase1_structures.md`

**目标**：创建 Proxy 和 Backend 交互的 C++ 结构体。

**产物**：
- `proxy/src/contracts/set_put_request.h`
  - `SetPullMode` 枚举
  - `SetPullSource` 结构体（含工厂函数）
  - `ProxySetPutBlockRequest` 请求结构
  - `ProxySetPutBlockResponse` 响应结构

**依赖**：无

**验证**：编译通过，工厂函数可用

---

### ✅ 阶段2：定义 Protobuf
**文件**：`proxy_backend_phase2_protobuf.md`

**目标**：定义 Proxy → Backend 的 RPC 接口。

**产物**：
- `proto/backend_service.proto`（Protobuf 定义）
- `proxy/src/contracts/set_put_converter.h`（C++ ↔ Protobuf 转换工具）

**依赖**：阶段1

**验证**：protobuf 编译通过，转换工具编译通过

---

### ✅ 阶段3：实现 Proxy 端 RPC 客户端
**文件**：`proxy_backend_phase3_proxy_rpc.md`

**目标**：实现 Proxy 向 Backend 发起 PutBlock RPC 的客户端。

**产物**：
- `proxy/src/backend_rpc.h/cpp`（BackendRpc 类）
- `proxy/src/common/rpc_base.h/cpp`（RPC 基类，如果没有）

**依赖**：阶段1、阶段2

**验证**：编译通过，可连接 Backend（即使 Backend 未实现）

---

### ✅ 阶段4：实现 Proxy 分块逻辑
**文件**：`proxy_backend_phase4_split_logic.md`

**目标**：实现 Proxy 的分块逻辑，将对象拆分为多个 block 并发上传。

**产物**：
- `proxy/src/put_handler.h/cpp`（PutHandler 类）
  - `HandleGdsPut()`：GDS 路径分块
  - `HandleUcxPut()`：UCX 路径分块
  - `ExecuteBlocks()`：并发执行
  - `AggregateResults()`：汇总结果

**依赖**：阶段3

**验证**：编译通过，单元测试验证分块逻辑

---

### ✅ 阶段5：实现 Backend 端服务
**文件**：`proxy_backend_phase5_backend_service.md`

**目标**：实现 Backend 的 PutBlock RPC 服务，拉取数据并写入存储。

**产物**：
- `backend/src/put_block_service.h/cpp`（PutBlockServiceImpl 类）
  - `PutBlock()` RPC 处理
  - `PullDataGds()`：GDS 拉取（TODO）
  - `PullDataUcx()`：UCX 拉取（TODO）
  - `ComputeCrc32c()`：CRC 计算（TODO）
  - `WriteToStorage()`：存储写入（TODO）
- `backend/src/main.cpp`（Backend 启动入口）

**依赖**：阶段2

**验证**：Backend 启动成功，可接收 RPC（模拟数据返回）

---

### ✅ 阶段6：集成到 Proxy 服务
**文件**：`proxy_backend_phase6_integration.md`

**目标**：将 PutHandler 集成到 Proxy 的 RPC 服务，对外提供完整接口。

**产物**：
- `proxy/src/proxy_service.h/cpp`（ProxyServiceImpl 类）
  - `GdsPut()` RPC 接口
  - `UcxPut()` RPC 接口
- `proxy/src/main.cpp`（Proxy 启动入口）

**依赖**：阶段4、阶段5

**验证**：端到端测试（Client → Proxy → Backend）

---

## 📊 依赖关系图

```
阶段1：数据结构
   │
   ├──> 阶段2：Protobuf
   │       │
   │       ├──> 阶段3：Proxy RPC 客户端
   │       │       │
   │       │       └──> 阶段4：Proxy 分块逻辑
   │       │               │
   │       │               └──> 阶段6：集成到 Proxy 服务
   │       │
   │       └──> 阶段5：Backend 服务
   │               │
   │               └──> 阶段6：集成到 Proxy 服务
   │
   └──> (阶段1 可以独立验证)
```

---

## 🚀 实施建议

### 推荐顺序

1. **阶段1 → 阶段2**：先定义数据结构和接口（编译验证）
2. **阶段3 → 阶段4**：实现 Proxy 端完整逻辑（可用 Mock Backend 测试）
3. **阶段5**：实现 Backend 端服务（可用 Mock 数据测试）
4. **阶段6**：集成并端到端测试

### 并行开发

- **Proxy 开发**：阶段1 → 阶段2 → 阶段3 → 阶段4 → 阶段6
- **Backend 开发**：阶段1 → 阶段2 → 阶段5 → 阶段6
- **交叉点**：阶段2（Protobuf 定义需要双方协商）

---

## 🔧 编译依赖

### 所有阶段共同依赖

- C++17 编译器（g++ 7+ 或 clang++ 5+）
- brpc（RPC 框架）
- protobuf（消息序列化）
- spdlog（日志库）
- gflags（命令行参数）

### Proxy 特有依赖

- 无额外依赖

### Backend 特有依赖（TODO 实现时需要）

- CUDA Toolkit（GDS 拉取）
- UCX（RDMA 拉取）
- 存储系统 SDK（写入接口）

---

## ✅ 验证清单

### 阶段1
- [ ] `set_put_request.h` 编译通过
- [ ] 工厂函数 `SetPullSource::Gds()` 可用
- [ ] 工厂函数 `SetPullSource::Ucx()` 可用

### 阶段2
- [ ] `backend_service.proto` 编译通过
- [ ] 生成的 C++ 代码包含 `SetPullSource` 消息
- [ ] 转换工具 `ToProto()` / `FromProto()` 编译通过

### 阶段3
- [ ] `BackendRpc` 类编译通过
- [ ] 可连接到 Backend 地址（即使未实现）

### 阶段4
- [ ] `PutHandler` 类编译通过
- [ ] `SplitToBlocksGds()` 正确计算 block 数量
- [ ] `ExecuteBlocks()` 可并发调用 Backend

### 阶段5
- [ ] Backend 服务启动成功（监听 8080）
- [ ] 可接收 PutBlock RPC 请求
- [ ] 返回模拟数据（ok=true, crc32c=xxx）

### 阶段6
- [ ] Proxy 服务启动成功（监听 9090）
- [ ] Client 可调用 Proxy 的 GdsPut
- [ ] Proxy 可转发到 Backend
- [ ] 端到端日志追踪完整（request_id 贯穿）

---

## 📝 后续 TODO

### 高优先级（核心功能）

1. **Backend GDS 拉取**：集成 cuFile API
2. **Backend UCX 拉取**：集成 UCX ucp_get
3. **CRC32C 计算**：使用硬件加速
4. **存储系统写入**：对接真实存储 API

### 中优先级（生产就绪）

5. **重试逻辑**：Backend 拉取失败时重试
6. **超时控制**：Backend 拉取超时处理
7. **负载均衡**：Proxy 选择 Backend 的策略优化
8. **Set 选择**：Proxy 根据负载选择 set_id

### 低优先级（优化）

9. **监控指标**：延迟、吞吐量、错误率
10. **日志优化**：结构化日志，便于解析
11. **性能调优**：并发度、内存池、零拷贝

---

## 📚 文档索引

1. [阶段1：定义数据结构](./proxy_backend_phase1_structures.md)
2. [阶段2：定义 Protobuf](./proxy_backend_phase2_protobuf.md)
3. [阶段3：实现 Proxy 端 RPC 客户端](./proxy_backend_phase3_proxy_rpc.md)
4. [阶段4：实现 Proxy 分块逻辑](./proxy_backend_phase4_split_logic.md)
5. [阶段5：实现 Backend 端服务](./proxy_backend_phase5_backend_service.md)
6. [阶段6：集成到 Proxy 服务](./proxy_backend_phase6_integration.md)

---

## 🎉 完成标志

当以下所有条件满足时，本重构完成：

- ✅ Client 可通过 Proxy 上传对象（GDS 路径）
- ✅ Client 可通过 Proxy 上传对象（UCX 路径）
- ✅ Backend 可从 Client 拉取数据（模拟实现）
- ✅ 端到端日志追踪完整
- ✅ 所有 TODO 项标记清晰，便于后续实现

**注意**：当前版本使用模拟数据（Backend 的 Pull/Write/CRC 逻辑），真实实现需要对接 CUDA、UCX 和存储系统。
