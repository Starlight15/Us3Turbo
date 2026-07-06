# Multipart Upload — 实现总结

本目录（`review/`）为分段上传（Multipart Upload）的设计文档。`multipart_README.md`
是总览，`multipart_phase{1..7}_*.md` 是分阶段实施 spec。下面记录实际落地的
实现状态、与 spec 的差异、以及端到端验证结果。

---

## 实现状态（v1，全部 7 阶段已落地）

| 阶段 | spec | 实际产物 | 状态 |
|------|------|----------|------|
| 1 | `multipart_phase1_proto.md` | `proto/control_plane.proto` 新增 4 RPC + `BackendDataPlane` 服务 + 全部消息 | ✅ |
| 2 | `multipart_phase2_session_manager.md` | `proxy/src/multipart/{upload_session.h,session_manager.{h,cpp}}` + `proxy/src/common/utils.{h,cpp}` | ✅ |
| 3 | `multipart_phase3_proxy_rpc.md` | `ProxyControlPlaneService` 新增 4 handler（直接走真实 handler，未留 mock 阶段） | ✅ |
| 4 | `multipart_phase4_proxy_split.md` | `proxy/src/multipart/multipart_put_handler.{h,cpp}`（4MB 切分 + `std::async` 并发） | ✅ |
| 5 | `multipart_phase5_backend_putblock.md` | `backend/src/backend_block_data_plane_service.{h,cpp}` + `BackendGdsSink` 加 `source_offset` 形参 | ✅ |
| 6 | `multipart_phase6_client_impl.md` | `Client` 新增 4 方法 + `ProxyRpc` 4 wrapper | ✅ |
| 7 | `multipart_phase7_integration.md` | `examples/gds/{gds,ucx}_multipart_example.cpp` + `multipart_bench_example.cpp` | ✅ |

---

## 与 spec 的关键差异（基于实际代码库 reconcile）

1. **日志**：spec 用 glog `LOG(INFO)`，实际代码库用 **spdlog**（`spdlog::info/error`）。
   全部按 spdlog 实现。
2. **C++ 标准**：spec 写 C++17，实际根 CMake 强制 **C++20**。`std::span`/`std::shared_mutex`
   等均可用。
3. **测试框架**：spec 用 gtest/benchmark，实际 `third_party` 无此依赖。Phase 7
   测试改为 **plain-`main()` 风格**（与既有 `gds_bench_example` 一致）。
4. **`BackendDataPlane` 第二服务注册**：spec 未说明如何在 backend 注册第二个 brpc
   service。实际 backend 用独立的 `BackendBlockDataPlaneService` 类（不能与 `Control`
   共类多继承——Service 多继承会产生 `CallMethod` 歧义），与 `BackendDataPlaneService`
   共注册于同一 `brpc::Server`（两 service descriptor full_name 不同，可共存）。
5. **block etag 算法**：spec 提“SHA1 base64”，但 block 字节在 backend 丢弃前已算
   CRC32C。为避免另开 SHA1 路径需保留字节，实际 block etag = `BuildEtag(crc32c)`
   （8 位十六进制串，与单步 `GdsPut/UcxPut` 的 etag 一致）；part/对象级 etag 仍走
   spec 的 “4 字节 LE count + SHA1(各 etag 拼接) base64”。
6. **GDS offset 透传**：`cuObjServer::handlePutObject` 的 `local_offset` 形参是“本地
   pinned buffer 内偏移”，**不是**远端 source_offset。实际把 `source_offset` 加到
   `remote_buf_start`（从 token 解析的远端基地址）上，`local_offset` 仍传 0。
7. **CRC 在多 block 时不汇总**：block crc 仅做传输校验；多 block part 的 `crc32c`
   不回填（单 block 时取该 block crc）。spec 的 “CRC XOR 汇总” 简化为不汇总。
8. **sink 方法扩展**：未新建 `ReceiveBlock`，而是给既有 `ReceiveAndDiscard` 加
   `source_offset` 默认形参（单步路径传 0，向后兼容）。

---

## 端到端验证（2026-07-03，.198 单机）

启动 backend（`--backend_compute_crc32c=true`）+ proxy，跑示例：

| 用例 | 结果 |
|------|------|
| GDS 20MB / 4 part × 5MB（`--verify-crc32c`） | ✅ `size=20971520`，吞吐 ~166 MiB/s |
| GDS 100MB / 20 part × 5MB | ✅ `size=104857600`，吞吐 ~156 MiB/s |
| 单步 vs 分段基准（20MB，3 reps 中位数） | 单步 81.6ms/245 MiB/s；分段 118ms/170 MiB/s |
| UCX 分段 | ⚠️ 卡在 `UcxPut`（client → proxy 段未到达 proxy 日志），**单步 UCX PUT 同样卡住**——属既有 UCX 链路的环境问题（UCX 1.16 vs 要求 1.18），非分段上传回归 |

> GDS 路径完整端到端通过（含 `source_offset` 透传：backend 日志可见 `off=0` 与
> `off=4194304` 两个 block 分别拉取，CRC 正确）。UCX 卡顿与本次改动无关——
> 单步 `ucx_put_example` 在同一环境同样超时，需先解决 UCX 运行期版本/配置问题。

---

## 已知限制（v1，沿用 spec）

- 会话状态仅存 Proxy 内存（`std::unordered_map`），Proxy 重启丢失。
- Backend 仍 discard（不写真实存储）。
- 无 `AbortMultipartUpload` / `ListParts`（靠 TTL 3 天过期清理）。
- Client 串行上传 part（无并发）。
- 单 part 失败不自动重试（Client 层 retry-once 仅作用于单步 `PutObject`）。
