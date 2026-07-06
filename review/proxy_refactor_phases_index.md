# Proxy 分层重构 — 阶段总览（4 个可闭环阶段）

把"上帝类" `ProxyControlPlaneService` 拆成 4 层：接口 / 服务 / 索引(mock) / 存储。
**每个阶段独立闭环**：可单独编译、跑通测试、行为等价后交付，再进下一阶段。

详细设计见 `proxy_layering_refactor.md`；各阶段执行提示词见下表。

---

## 阶段划分

| 阶段 | 文件 | 做什么 | 闭环标志 |
|---|---|---|---|
| **1** | `proxy_refactor_phase1_status_and_dir.md` | 建 `common/status.h`；接口层 `service/` → `api/` 目录迁移 | 纯移动，编译过 + 测试绿 |
| **2** | `proxy_refactor_phase2_service_layer.md` | 抽 `SinglePutService`/`MultipartService`；接口层改薄委托 | 服务层无 brpc，handler ≤20 行 |
| **3** | `proxy_refactor_phase3_index_layer.md` | 抽 `IUploadIndex`+`InMemoryUploadIndex`(mock)；业务规则上移服务层；删 SessionManager | 索引层纯 CRUD，服务层只依赖接口 |
| **4** | `proxy_refactor_phase4_storage_layer.md` | 抽 `BackendGateway`/`BlockStorage`，channel 下沉；main 装配；清空 multipart/ | 接口层无 channel，全目标达成 |

---

## 依赖关系（严格依序）

```
阶段1 (status.h + 目录)
   └─▶ 阶段2 (服务层，依赖 status.h)
          └─▶ 阶段3 (索引层，服务层改依赖 IUploadIndex*)
                 └─▶ 阶段4 (存储层，服务层改依赖 Gateway/BlockStorage*)
```
每阶段都在上一阶段可编译的基础上推进；不可跳阶段（后阶段引用前阶段产物）。

---

## 全程不变的约束（每阶段验收都查）

1. **GDS/UCX 不共享代码**：每层两条链路各自独立方法，改一条不影响另一条。
   仅纯算法（`utils::CombineETags`/`NowMs`）可共用。
2. **brpc 单 service**：接口层始终是唯一 `Control` 子类，内部委托。
3. **行为等价**：16MiB 限制、串行 block、part 升序无重复、final etag 算法、
   成功后清理、Abort 幂等——4 个阶段全程保持。
4. **既往 20 条规范**：注释 ≤1 行/块、无死代码、错误码用到再加、`[[nodiscard]]` 等。

---

## 最终目录（阶段 4 完成后）
```
proxy/src/
  api/       proxy_control_plane_service.{h,cpp}    # 接口层
  service/   single_put_service.{h,cpp}             # 服务层
             multipart_service.{h,cpp}
  index/     upload_index.h                          # 索引层(接口)
             in_memory_upload_index.{h,cpp}          #   mock 实现
  storage/   backend_gateway.{h,cpp}                # 存储层(单步)
             block_storage.{h,cpp}                  #   (block 切分)
  common/    status.h  errors.h  utils.{h,cpp}
  main.cpp                                          # 依赖注入装配
```

## 达成的可拓展性
- 索引层：内存 mock → MongoDB(`us3_minit`/`us3_partlist`) 只需实现 `IUploadIndex`，服务层零改动。
- 存储层：backend 传输可替换，不影响服务层。
- 每层可独立单测（服务层 mock 索引/存储，索引层直接测 CRUD）。
