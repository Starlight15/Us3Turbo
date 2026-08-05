# 重构 Prompt:proxy 生成 trace_id,显式透传,client 不缓存

## 0. 你是谁、要做什么
你是这名仓库的 C++ 工程师。当前代码已回退到干净 baseline:**只有 `request_id`/`req_id`(每次 RPC 新建、client 生成),multipart 用 proxy 生成的 `upload_id` 做会话句柄;没有任何 trace_id,也没有任何 client 端 id map**。

本次任务:为「跨多次 RPC 的逻辑操作」引入一个 **proxy 生成、client 显式透传、绝不缓存** 的 `trace_id`,用于跨端/跨 RPC 日志关联。覆盖 GET 与 multipart 两条多步链路。单步 PUT 不引入 trace_id。

## 1. 背景与动机(不要改设计,只理解)
- `request_id`(代码里常叫 `req_id`):**每次具体 RPC** 一个,client 端 `detail::MakeReqId()` 生成,区分重试。**保留不动**。
- `trace_id`(本次新增):**一次逻辑会话**(GET 的 stat→get;multipart 的 create→upload×N→complete/abort)一个,**proxy 生成**(UUID v4),随首个 RPC 的响应回到 client,client 在后续 RPC 里**作为参数显式透传**回去。用途是跨 RPC、跨端日志关联。
- 旧版失败教训:曾让 client 自己生成 trace_id 并用 `multipart_traces_`/`get_traces_` 两个带锁 map 缓存,GET 的 map 还按 `bucket/key` 做 key —— 既泄漏(只写不删)又会撞 key 串线。**本次严禁重新引入任何 client 端 id map / 缓存 / 锁。** trace_id 一律以参数形式流过调用栈,caller 用局部变量持有。

## 2. 硬规则(违反即错)
1. **trace_id 一律 proxy 生成**(`us3_turbo::proxy::utils::GenUuid()`)。client 永不生成 trace_id。
2. **client 端不得出现任何 trace_id 的 map / 成员 / 锁 / 缓存。** 它只能是函数参数或局部变量。
3. **首个 RPC 的 request 不带 trace_id**(client 此时没有);response 带回 proxy 生成的 trace_id。后续 RPC 的 request 由 client 透传 trace_id。
4. **单步 PUT(`GdsPut`/`RdmaPut`,以及 `Client::PutObjectGds`/`PutObjectRdma`、`GdsPutChannel`/`RdmaPutChannel`)不引入 trace_id**,签名不动。它是单次 RPC,req_id 已够。
5. **保持现有公共 API 风格**:StatObject 仍以 `out_object_size` 为返回,CreateMultipartUpload 仍以 `out_upload_id` 为返回;trace_id 作为**额外的 out/in 参数**追加,不引入 handle 结构体替换现有签名。
6. `upload_id` 仍是 multipart 的功能性会话句柄(proxy 用它查会话状态),**不变**;trace_id 与之并列,仅做日志关联。
7. 遵循仓库现有风格:`[[nodiscard]]`、`std::string_view` 入参 / `std::string&` 出参、`const std::string&` 入参、中文 doxygen 注释、4 空格缩进、`LOG_INFO/LOG_ERROR(req_id, ...)` 日志风格。

## 3. trace_id 生命周期(务必照此实现)
```
GET:
  Client::StatObject  ──req_id──▶  ProxyRpc::StatObject ──▶ proxy StatObject handler
                                                       GenUuid() 生成 trace_id
                                  ◀──resp.trace_id────  set into response
  caller 持有 trace_id 局部变量
  Client::GetObject{Gds,Rdma}(trace_id) ──▶ ProxyRpc ──▶ proxy GdsGet/RdmaGet handler
                                                                  从 request 取 trace_id 记日志

Multipart:
  Client::CreateMultipartUpload ──▶ proxy CreateMultipartUpload handler
                                     GenUuid() 生成 trace_id + multipart_->CreateUpload 生成 upload_id
                  ◀──resp.trace_id + resp.upload_id──
  caller 持有 trace_id 局部变量
  Client::UploadPart{Gds,Rdma}/Complete/Abort(trace_id, upload_id, ...) ──▶ proxy 取 trace_id 记日志
```
失败路径也要在日志里带上 trace_id(在 handler 开头生成后即可),成功路径才 `set_trace_id` 进 response。

## 4. 逐文件改动清单

### 4.1 proto (`Us3Turbo/proto/control_plane.proto`)
新增字段(注意 field 号不要与现有冲突):
- `StatObjectResponse`:新增 `string trace_id = 6;`(现 ok=1,error_message=2,object_size=3,block_size=4,hash=5)。注释:proxy 生成,供后续 GetObject 透传做日志关联。
- `ClientProxyGetRequest`:新增 `string trace_id = 5;`(现 request_id=1,bucket=2,key=3,object_size=4,gds_source=10,rdma_source=12)。
- `CreateMultipartUploadResponse`:新增 `string trace_id = 4;`(现 ok=1,upload_id=2,error_message=3)。
- `UploadPartGdsRequest`:新增 `string trace_id = 6;`(现 request_id=1,upload_id=2,part_number=3,part_size=4,rdma_token=5)。
- `UploadPartRdmaRequest`:新增 `string trace_id = 6;`(同上布局)。
- `CompleteMultipartUploadRequest`:新增 `string trace_id = 4;`(现 request_id=1,upload_id=2,parts=3)。
- `AbortMultipartUploadRequest`:新增 `string trace_id = 3;`(现 request_id=1,upload_id=2)。
- **不**给 `StatObjectRequest`/`CreateMultipartUploadRequest`/`ClientProxyPutRequest` 加 trace_id。
改完按仓库现有方式重新生成 pb(参考现有 build/编译流程,确认 `control_plane.pb.h/.cc` 更新)。

### 4.2 proxy handler (`Us3Turbo/proxy/src/api/proxy_service.cpp`)
- `StatObject`:开头生成 `const std::string trace_id = utils::GenUuid();`,在 start/fail/success 日志里带上 `trace_id={}`;成功分支 `response->set_trace_id(trace_id);`。
- `GdsGet` / `RdmaGet`:从 `request->trace_id()` 取,在 start/fail/success 日志里带上。
- `CreateMultipartUpload`:开头生成 `const std::string trace_id = utils::GenUuid();`,日志带上;成功分支 `response->set_trace_id(trace_id);`(`upload_id` 仍由 `multipart_->CreateUpload` 产生并 `set_upload_id`)。
- `UploadPartGds`/`UploadPartRdma`/`CompleteMultipartUpload`/`AbortMultipartUpload`:从 `request->trace_id()` 取,日志带上。
- 单步 `GdsPut`/`RdmaPut` handler:**不动**。
`utils::GenUuid` 已存在(`proxy/src/common/utils.h`),handler 已 include utils(用到了 `utils::ElapsedMs`)。

### 4.3 proxy service 层
- `get_object.h/.cpp`、`multipart.h/.cpp`:**不需要改**。trace_id 是 handler 层的日志/响应字段,不进 service 层逻辑。

### 4.4 ProxyRpc (`Us3Turbo/client/src/rpc/proxy_rpc.h` + `.cpp`)
签名追加 trace_id(单步 `GdsPut`/`RdmaPut` 不动):
```cpp
// StatObject 增加 out_trace_id 出参
[[nodiscard]] bool StatObject(std::string_view req_id, const std::string& bucket,
                              const std::string& key, std::uint64_t& out_object_size,
                              std::string& out_trace_id, std::string& out_error) const;
// GdsGet/RdmaGet 增加 trace_id 入参(放在 req_id 之后)
[[nodiscard]] bool GdsGet(std::string_view req_id, std::string_view trace_id,
                           const std::string& bucket, const std::string& key,
                           std::uint64_t object_size, const std::string& rdma_token,
                           GetPathResult& res) const;
[[nodiscard]] bool RdmaGet(std::string_view req_id, std::string_view trace_id,
                           const std::string& bucket, const std::string& key,
                           std::uint64_t object_size, const std::string& rdma_token,
                           GetPathResult& res) const;
// Create 增加 out_trace_id 出参
[[nodiscard]] bool CreateMultipartUpload(std::string_view req_id, const std::string& bucket,
                                         const std::string& key,
                                         ::us3_turbo::proxy::PutDataPath path,
                                         std::string& out_upload_id, std::string& out_trace_id,
                                         std::string& out_error) const;
// UploadPart{Gds,Rdma} 增加 trace_id 入参(放 req_id 之后、upload_id 之前)
[[nodiscard]] bool UploadPartGds(std::string_view req_id, std::string_view trace_id,
                                 const std::string& upload_id, std::uint32_t part_number,
                                 std::uint64_t part_size, const std::string& rdma_token,
                                 PutPathResult& res) const;
[[nodiscard]] bool UploadPartRdma(std::string_view req_id, std::string_view trace_id,
                                  const std::string& upload_id, std::uint32_t part_number,
                                  std::uint64_t part_size, const std::string& rdma_token,
                                  PutPathResult& res) const;
// Complete 增加 trace_id 入参
[[nodiscard]] bool CompleteMultipartUpload(std::string_view req_id, std::string_view trace_id,
                                           const std::string& upload_id,
                                           const std::vector<std::pair<std::uint32_t,std::string>>& parts,
                                           CompletedMultipart& out) const;
// Abort 增加 trace_id 入参
[[nodiscard]] bool AbortMultipartUpload(std::string_view req_id, std::string_view trace_id,
                                        const std::string& upload_id, std::string& out_error) const;
```
`.cpp` 实现里:
- `StatObject`:成功后 `out_trace_id = resp.trace_id();`(`resp.ok()` 为真时;失败按现有逻辑置 out_error 即可,trace_id 留空)。
- `GdsGet`/`RdmaGet`:`rpc_request.set_trace_id(std::string(trace_id));`。
- `CreateMultipartUpload`:成功后 `out_trace_id = resp.trace_id();`。
- `UploadPart*`/`Complete`/`Abort`:`req.set_trace_id(std::string(trace_id));`。
其余逻辑一字不动。

### 4.5 GdsGetChannel (`Us3Turbo/client/src/transport/gds_get_channel.h` + `.cpp`)
```cpp
[[nodiscard]] bool StatObject(const std::string& bucket, const std::string& key,
                              std::uint64_t& out_object_size, std::string& out_trace_id,
                              std::string& out_error) const;
[[nodiscard]] bool GetOnce(const std::string& bucket, const std::string& key,
                           std::string_view trace_id, MutableBufferView buffer,
                           GetPathResult& res) const;
```
实现透传给 `proxy_`(StatObject 把 `out_trace_id` 传出;GetOnce 把 `trace_id` 传进 `proxy_.GdsGet`)。

### 4.6 RdmaGetChannel (`Us3Turbo/client/src/transport/rdma_get_channel.h` + `.cpp`)
```cpp
[[nodiscard]] bool GetOnce(const std::string& bucket, const std::string& key,
                           std::string_view trace_id, MutableBufferView buffer,
                           GetPathResult& res) const;
```
透传给 `proxy_.RdmaGet`。(RdmaGetChannel 没有 StatObject;StatObject 走 GdsGetChannel,见 4.7。)

### 4.7 Client 公共 API (`Us3Turbo/client/include/us3_turbo/client/client.h` + `src/client.cpp`)
```cpp
[[nodiscard]] bool StatObject(const std::string& bucket, const std::string& key,
                              std::uint64_t& out_object_size, std::string& out_trace_id,
                              std::string& out_error) const;
[[nodiscard]] bool GetObjectGds(const std::string& bucket, const std::string& key,
                                 std::string_view trace_id, MutableBufferView buffer,
                                 GetPathResult& res) const;
[[nodiscard]] bool GetObjectRdma(const std::string& bucket, const std::string& key,
                                 std::string_view trace_id, MutableBufferView buffer,
                                 GetPathResult& res) const;

[[nodiscard]] bool CreateMultipartUpload(const std::string& bucket, const std::string& key,
                                         PutDataPath path, std::string& out_upload_id,
                                         std::string& out_trace_id, std::string& out_error) const;
[[nodiscard]] bool UploadPartGds(const std::string& upload_id, std::string_view trace_id,
                                 std::uint32_t part_number, ConstBufferView buffer,
                                 std::string& out_etag, std::string& out_error) const;
[[nodiscard]] bool UploadPartRdma(const std::string& upload_id, std::string_view trace_id,
                                  std::uint32_t part_number, ConstBufferView buffer,
                                  std::string& out_etag, std::string& out_error) const;
[[nodiscard]] bool CompleteMultipartUpload(const std::string& upload_id, std::string_view trace_id,
                                           const std::vector<PartInfo>& parts,
                                           CompletedMultipart& out) const;
[[nodiscard]] bool AbortMultipartUpload(const std::string& upload_id, std::string_view trace_id,
                                       std::string& out_error) const;
```
`src/client.cpp` 实现要点:
- `StatObject`:调 `gds_get_channel_->StatObject(bucket, key, out_object_size, out_trace_id, out_error)`,把 `out_trace_id` 传出。(StatObject 仍优先 GDS、回退 RDMA 的现有判断保留。)
- `GetObjectGds`/`GetObjectRdma`:把 `trace_id` 透传给对应 channel 的 `GetOnce`。
- `CreateMultipartUpload`:调 `proxy_->CreateMultipartUpload(req_id, bucket, key, proto_path, out_upload_id, out_trace_id, out_error)`,把 `out_trace_id` 传出。
- `UploadPart*`/`Complete`/`Abort`:把 `trace_id` 透传给 `proxy_`。
- **删除/不要新增**任何 `multipart_traces_`/`get_traces_` 之类成员、锁、helper(baseline 已无,保持无)。
- 单步 `PutObjectGds`/`PutObjectRdma`:**不动**。

### 4.8 调用方迁移(`rtest/` 下所有 GET 与 multipart 用例)
调用方现在需要在「首个 RPC」拿到 trace_id 局部变量,「后续 RPC」透传。示例迁移模式:

GET(`rtest/examples/gds/gds_get_example.cpp` 等):
```cpp
std::uint64_t obj_size = 0;
std::string trace_id;
std::string stat_err;
client.StatObject(kBucket, "gds-get-demo", obj_size, trace_id, stat_err);
// ...
GetPathResult get_res;
client.GetObjectGds(kBucket, "gds-get-demo", trace_id,
                    MutableBufferView{.data = dev_get, .size = obj_size}, get_res);
```
Multipart(各 multipart 用例):
```cpp
std::string upload_id, trace_id, err;
client.CreateMultipartUpload(bucket, key, PutDataPath::kGds, upload_id, trace_id, err);
// 每个 part:
client.UploadPartGds(upload_id, trace_id, part_no, buf, etag, err);
// ...
client.CompleteMultipartUpload(upload_id, trace_id, parts, out);
// 失败清理:
client.AbortMultipartUpload(upload_id, trace_id, err);
```
需要更新的文件(grep 确认全覆盖,不要漏):
`rtest/examples/gds/gds_get_example.cpp`、`rtest/examples/rdma/rdma_get_example.cpp`、`rtest/regression/gds/test_get_single_block_crc.cpp`、`rtest/regression/gds/test_get_multi_block_hash.cpp`、`rtest/regression/gds/test_multipart_single_part.cpp`、`rtest/regression/gds/test_put_single.cpp`(若含 multipart 调用)、`rtest/regression/rdma/test_get_single_block_crc.cpp`、`rtest/regression/rdma/test_get_multi_block_hash.cpp`、`rtest/bench/rdma/rdma_get_bench.cpp`,以及任何其他调用上述 Client/ProxyRpc/Channel 方法的地方。

## 5. 代码风格核对(对照周围代码)
- 头文件 `#pragma once`、include 顺序(项目头、第三方、系统)与现有文件一致;新增字段需要的头文件(若用到)按现有习惯加。
- 中文 doxygen `/** @brief ... */` 注释;字段/参数注释风格与 proto 既有注释一致。
- 出参用 `std::string&`/`std::uint64_t&`,入参 id 用 `std::string_view`(短字符串)或 `const std::string&`(与周围一致;ProxyRpc 入参 trace_id 用 `std::string_view` 与 req_id 对齐)。
- `[[nodiscard]]` 保留在所有 bool 方法上。
- 不要引入未使用的 include;不要改变现有命名空间结构。

## 6. 验证(完成后逐项确认)
1. `grep -rn "trace_id" Us3Turbo/client/include Us3Turbo/client/src` —— 只应出现在函数签名/局部变量/透传处,**不得**出现类成员、`std::unordered_map`、`std::mutex`、`mutable` 与 trace 相关的字段。
2. `grep -rn "multipart_traces_\|get_traces_\|GenTraceId\|LookupOrGenTrace\|LookupOrGenGetTrace" Us3Turbo` —— 应为空(baseline 本就无,确认未复活)。
3. `grep -rn "GenUuid" Us3Turbo/proxy/src/api/proxy_service.cpp` —— 出现在 StatObject 与 CreateMultipartUpload 两个 handler。
4. 单步 PUT 链路签名未变:`ProxyRpc::GdsPut/RdmaPut`、`Client::PutObjectGds/PutObjectRdma`、`GdsPutChannel`/`RdmaPutChannel` 均无 trace_id。
5. proto 重新生成成功,新 field 号无冲突。
6. 按仓库现有构建方式编译 client + proxy + rtest,全部通过;跑 GET 与 multipart 回归用例通过。
7. 日志可验证:同一 GET 会话的 StatObject 与 GdsGet/RdmaGet 日志出现同一个 trace_id;同一 multipart 会话的 Create 与 UploadPart/Complete 日志出现同一个 trace_id。

## 7. 不要做的事
- 不要给单步 PUT 加 trace_id。
- 不要在 client 端缓存/映射 trace_id(任何 map、成员、锁)。
- 不要让 client 生成 trace_id(只在 proxy `GenUuid`)。
- 不要改动 `upload_id` 的生成与语义。
- 不要顺手重构无关代码、不要改日志库、不要动数据面。

完成全部改动后,按第 6 节逐项自查并报告结果。
