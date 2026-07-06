# Multipart 新增代码评审 — 对照规范 + 冗余/复杂度

评审范围：`proto/control_plane.proto`（新增消息）、`proxy/src/multipart/*`、`proxy/src/common/utils.*`、
`proxy/src/service/proxy_control_plane_service.*`（分段部分）、`backend/src/backend_block_data_plane_service.*`、
`client/src/rpc/proxy_rpc.*`（分段部分）、`client/src/client.cpp`（分段部分）。

对照要求见 `review/code_style_requirements`（20 条）。每条标注 **严重度**（🔴必改 / 🟡建议 / 🟢可选）与违反的**要求编号**。

---

## A. 重复代码（req #4 死代码/冗余、#7 薄抽象内联、#8 折叠薄文件）

### A1 🔴 ETag 汇总逻辑逐字节重复两份 — req #4
`session_manager.cpp:138 ComputeFinalETag` 与 `multipart_put_handler.cpp:131 Aggregate` 里
"4 字节 LE count 前缀 + SHA1(拼接) + base64" 是**完全相同**的代码块。
- **改法**：抽到 `utils`：`std::string CombineETags(const std::vector<std::string>& etags)`，
  两处都调它。单元素直接返回、多元素走前缀+SHA1+base64 的分支也一并收进去。
- **收益**：消除双份维护；将来对齐 s3proxy 格式（阶段 9）只改一处。

### A2 🔴 `CompletedMultipart` 结构重复定义 — req #7 / #11
`ProxyRpc::CompletedMultipart`（proxy_rpc.h:112-118）与 `Client::CompletedMultipart`
（client.h:56-62）字段**完全一致**（ok/object_id/etag/object_size/error），
`client.cpp:352-356` 再手工逐字段搬运。
- **改法**：二者合一——client.h 用 `using CompletedMultipart = ProxyRpc::CompletedMultipart;`
  或把该 struct 提到共享的 types 头，client 层直接透传，删掉逐字段拷贝。

### A3 🟡 `PartInfo` 与 pair 向量的重复转换 — req #6 / #7
`Client::PartInfo`（client.h:65-68）→ `client.cpp:340-344` 转成
`std::vector<std::pair<uint32,string>>` 再传 ProxyRpc。两层表示同一数据。
- **改法**：让 `ProxyRpc::CompleteMultipartUpload` 直接收 `PartInfo` 向量，省掉 proto_parts 转换。

### A4 🟡 `NowMs()` 重复定义两处 — req #4
`proxy_control_plane_service.cpp:24` 与 `session_manager.cpp:14` 各有一份同样的 `NowMs()`。
- **改法**：入 `utils`（如 `utils::NowMs()`），两处共用。

---

## B. 函数过复杂 / 结构重复（req #7 简化控制流、#9 inline 中间层）

### B1 🟡 `HandleGdsPart` / `HandleUcxPart` 骨架重复 — multipart_put_handler.cpp:149-221
两函数除 lambda 内调 `CallBackendPutBlockGds` 还是 `...Ucx` 外，
SplitToBlocks→起 futures→collect→Aggregate 骨架**逐行相同**（~35 行 ×2）。
- **改法**：抽一个私有骨架，把"单 block 调用"作为 `std::function`/模板 callable 参数化：
  ```cpp
  PartResult RunPart(part_size, std::function<Resp(const BlockPlan&)> call_block);
  ```
  HandleGds/Ucx 各自只传一个绑定好 source 的 lambda。
- **注意**：若与"GDS/UCX 两链路不抽象"原则冲突，可只抽**并发收集骨架**（与 source 无关），
  仍保留两个 Handle 入口。请评估。

### B2 🟡 `CallBackendPutBlockGds` / `...Ucx` 尾部重复 — multipart_put_handler.cpp:38-97
两函数末尾 controller/timeout/`PutBlock`/失败回填 4 行**完全相同**，只有 req 的 source 填充不同。
- **改法**：抽 `Resp CallBackend(const ProxyBackendPutBlockRequest& req)` 处理公共调用+失败回填，
  Gds/Ucx 只负责组装 req。

### B3 🟡 `GdsPut` / `UcxPut` resp 回填重复 — proxy_rpc.cpp:65-71 / 109-115
两处 6 行 `result.xxx = resp.xxx()` 完全一样。
- **改法**：抽 `static void FillPutResult(PutPathResult&, const proxy::PutPathResult&)`。
- **注意**：同样受"两链路不抽象"约束，这是**纯 resp→struct 映射**、与链路语义无关，
  倾向于可抽。请拍板。

---

## C. 薄封装 / 冗余声明（req #5 删冗余前向声明、#7 薄抽象内联）

### C1 🔴 冗余前向声明 — proxy_control_plane_service.h:19
line 14 已 `#include "proxy/src/multipart/multipart_put_handler.h"`，
line 19 又 `class MultipartPutHandler;`。前向声明多余，删。

### C2 🟡 `ForwardResult` 是单行 `CopyFrom` 的薄封装 — proxy_control_plane_service.cpp:85-88
只被 2 处调用、函数体仅 `response->CopyFrom(bresp)`。
- **改法**：直接内联 `response->CopyFrom(bresp)`，删函数（req #7）。

### C3 🟢 `stub()` getter 薄封装 — proxy_rpc.h:130
`Control_Stub* stub() const { return stub_.get(); }` 只在本类 cpp 内用。
- **改法**：直接用 `stub_.get()`，删 getter。可选。

---

## D. 潜在正确性 / 性能

### D1 🟡 每 block 起 `std::async(std::launch::async)` — multipart_put_handler.cpp:166 / 203
在 brpc bthread 上下文里，`std::async(launch::async)` 起的是 **OS pthread** 做同步 RPC，
阻塞的是 pthread 而非 bthread，且有线程创建开销。part 内 block 数虽小（≤4，因 part≤16MB），
但属反模式。
- **改法**：改用 brpc 异步 `PutBlock`（传 `Closure` + `bthread::CountdownEvent` 收集），
  或串行调用（block 数小时串行可能更简单且无线程开销）。请评估是否值得改。

### D2 🟡 UUID 种子表达式可疑 — utils.cpp:30-33
`(std::random_device{}() << 1) ^ steady_clock(...)`：random_device 结果左移 1 位丢高位随机性，
写法无必要。
- **改法**：直接 `std::random_device{}()` 或 `std::seed_seq` 填种子；去掉左移。

### D3 🔴 `BuildBlockEtag` crc==0 分支注释与代码不符 — backend_block_data_plane_service.cpp:17-26
（已在 `review/multipart_phase8_etag_crc_fix.md` 记录）恒返回 `"block-0"`，注释声称用 block_no+size。
与阶段 8 一并修。

---

## E. 注释 / 风格（req #1 注释≤1行、#16-20 格式）

### E1 🟡 `GenUuid` 内注释偏多 — utils.cpp:28-41
3 处多行注释可压到 ≤1 行/块（req #1）。RFC 4122 version/variant 一行足够。

### E2 🟢 类注释过长 — proxy_control_plane_service.h:21-44（24 行）
信息量大但含重复解释（brpc 单实例约束讲了两遍）。保留核心约束、删重复。req #1 精神。

### E3 🟢 backend_block_data_plane_service.h 顶部注释 15 行
可压缩，核心留"复用 sink.ReceiveAndDiscard + block etag 语义"即可。

---

## 汇总（按建议处理顺序）

| # | 位置 | 严重度 | 类型 | 要求 |
|---|---|---|---|---|
| A1 | ComputeFinalETag / Aggregate | 🔴 | 重复→抽 utils | #4 |
| A2 | CompletedMultipart ×2 | 🔴 | 重复 struct | #7/#11 |
| C1 | service.h:19 前向声明 | 🔴 | 冗余声明 | #5 |
| D3 | BuildBlockEtag | 🔴 | 注释≠代码 | 阶段8 |
| A3 | PartInfo 转换 | 🟡 | 冗余转换 | #6/#7 |
| A4 | NowMs ×2 | 🟡 | 重复 helper | #4 |
| B1 | HandleGds/UcxPart | 🟡 | 骨架重复 | #7/#9 |
| B2 | CallBackendPutBlock×2 | 🟡 | 尾部重复 | #7 |
| B3 | GdsPut/UcxPut 回填 | 🟡 | 映射重复 | #7（受不抽象约束） |
| C2 | ForwardResult | 🟡 | 薄封装 | #7 |
| D1 | per-block std::async | 🟡 | 性能反模式 | — |
| D2 | UUID 种子 | 🟡 | 写法可疑 | — |
| E1 | GenUuid 注释 | 🟡 | 注释过多 | #1 |
| C3/E2/E3 | getter/类注释 | 🟢 | 可选精简 | #1/#7 |

**需你拍板的取舍**：
- **B1/B3** 与"GDS/UCX 两链路不抽象"原则冲突——纯映射/收集骨架（与链路语义无关）我倾向可抽，
  但如果你坚持零抽象，则保留现状、仅做 A/C/D 类。
- **D1** 是否改并发模型（std::async → brpc 异步 / 串行）取决于你对当前性能是否满意。
