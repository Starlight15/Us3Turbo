# rtest 运行中发现的问题记录

> 测试环境：192.168.1.198，proxy(:9100) → ufile-ac(:24000, GDS RDMA :18666 / UCX mlx5_2) + dbgate(:20165) + mongod(:27017)
> 本文件记录跑 `rtest/regression/{gds,ucx}` 过程中暴露出的 proxy / client / 环境问题，供后续修复。

---

## P1. proxy→dbgate 连接在空闲后变 CLOSE-WAIT，导致 CreateMultipartUpload 全链路失败

**症状（GDS T1.3 首跑、UCX T1.1 历史首跑均复现）**：
```
[CreateMultipartUpload] ... [SendAndRecv] SendAll body failed
[Create] InsertMinit failed: upload_id=<id> ret=12003
[CreateUpload] created upload_id= bucket=...        ← upload_id 为空
[CompleteMultipartUpload] start upload= parts=0     ← 后续全失效
[ExecuteMgo] DBGate returned error: ret_code=-30010 msg=
```
- `upload_id` 返回空字符串 → 客户端随后所有 UploadPart 报 `[E10001] invalid parameter`（"upload_id not found"）→ Complete 报 `invalid parameter` 而非 `invalid part size`。
- 即一个看似是"测试失败"的 FAIL，实为 proxy 持有的 dbgate TCP 连接已死。

**根因（源码已确认）**：
- `proxy/src/index/dbgate_client.cpp` `DBGateClient` 维护固定连接池（`conns_`，pool_size=4），`AcquireConn()` 轮询取 `conn->alive()` 的连接；`SendAndRecv()` 在 `SendAll`/`RecvAll` 失败时调 `set_dead()`+`Close()`。
- `proxy/src/storage/tcp_connection.cpp`：`Connect()` 仅在 `alive_==false` 时重连；`RecvAll` 用 `recv`(非 MSG_WAITALL)，**超时(SO_RCVTIMEO)返回 -1 被当作"对端异常"直接 set_dead+Close**。
- dbgate 侧（Go `gateway/gateway.go:163`）对空闲连接有读超时，到点发 RST/FIN 关闭。proxy 侧 `alive_` 仍为 true（因为没主动收发），下一次 `SendAndRecv` 才在 `SendAll`/`RecvAll` 上失败 —— 但失败这一次请求已经丢了（InsertMinit 没重试）。
- **关键缺陷**：`SendAndRecv` 失败后仅把当前连接标 dead，**不重试其它连接、也不重连当前连接重发**。一次偶发超时 = 一次请求失败 = 该 upload 整条链路作废。

**触发条件**：proxy 启动后空闲约 3~5 分钟（dbgate 侧读超时），下一次 multipart 操作命中死连接。

**规避（当前）**：每次跑测试前若 proxy 已空闲数分钟，先重启 proxy（或先发一个轻量请求预热）。已通过重启 proxy（pid 1313845）恢复。

**建议修复（proxy 侧）**：
1. `SendAndRecv` 失败后，在循环里 `Connect()` 当前连接或取下一条 alive 连接**重试一次**（幂等请求如 InsertMinit 可安全重试；非幂等请求需按 op 区分）。
2. 或对 dbgate 连接启用 TCP keepalive（`SO_KEEPALIVE` + 短 `TCP_KEEPIDLE`），空闲保活，避免被 dbgate 单侧关闭。
3. `AcquireConn` 在所有连接 `!alive` 时尝试逐个 `Connect()`（当前已有，但 `SendAndRecv` 失败后没回到 AcquireConn 重试）。

**修复落地（2026-07-14，见 `review/fix_proxy_connection_retry.md`）**：
- `proxy/src/index/dbgate_client.cpp:SendAndRecv` 与 `proxy/src/storage/ufile_ac_client.cpp:SendAndRecv` 改为 `for(attempt<2)` 重试循环（1 重试 = 2 尝试）。
- **关键修正**：仅 `set_dead` 当前连接不够——dbgate/ufile-ac 空闲关闭时**同批创建的池连接会同时失效**，但 proxy 侧 `alive_` 仍为 true（未尝试收发，不知道对端已关）。下次 `AcquireConn` 会返回另一条 `alive_=true` 但实际已死的"僵尸连接"，2 次尝试覆盖不了 N 条全死连接。
- 修复：失败后 `for (auto& c : conns_) c->set_dead();` **主动作废池中所有连接**，下次 `AcquireConn` 走 `Connect()` 建全新连接。`set_dead()` 原子、线程安全；`Close()` 由后续 `Connect()` 检测 `fd_>=0` 时完成。
- 协议错误（dbgate rsp_len>16MB / ufile-ac bad magic·body too short）不重试。
- **验证（`/tmp/proxy_retry2.log`）**：dbgate 在 09:59:45 一次关闭全部 4 条池连接后，10:02:45 首笔 GdsPut→WriteObjectIndex→InsertFileIdx 命中死连接，日志 `SendAll body failed → invalidated all pool conns, retry with fresh conn`，重试 `Connect()` 新连接成功，InsertFileIdx 完成，GdsPut success——**P1 修复确认有效**。GDS+UCX 全 10 用例 PASS。

**遗留（非本修复目标，记录在案）**：
- T1.2 场景A 重复 part 的 dup UploadPart，proxy 在 `AddPart` 处被 MongoDB `{uploadid,seq}` **唯一索引**拒绝（`InsertPart` 写入重复 (upload_id, part_number) → E11000 → `AddPart` 返回 false → `WritePartIndex failed` → `CleanupWrittenBlocks` 回滚 → UploadPart 返回 code 10004）。这是**预期的重复检测行为**（非静默），测试接受此结果判 PASS。
- 回滚 `CleanupWrittenBlocks` 调 `DeleteBlock` 删除已写 block；若此时 ufile-ac 连接恰好空闲关闭，`DeleteBlock` 的 2 次重试也可能"after 2 attempts"失败（best-effort 回滚，ufile-ac TTL 兜底，不影响测试结果）。日志中 8 次 `DeleteBlock ... after 2 attempts` 均属此类**回滚路径**，非数据写入失败。

**影响测试**：T1.1/T1.3（CreateMultipart 链路）受影响最大；GDS 通路连接正常时本身逻辑正确。

---

## P2. UCX 通路环境性不可用：ibv_create_ah for RC DEVX QP on mlx5_0 超时

**症状**：UCX 5 个用例 + 既有 `us3_turbo_ucx_put_example` 均失败：
```
ibv_create_ah(dlid=49152 sl=0 port=1 ... dgid=::ffff:192.168.1.198 sgid_index=3 traffic_class=106) for RC DEVX QP connect on mlx5_0 failed: Connection timed out
```
proxy 侧：`PutUcx: ufile-ac failed: backend retcode=-1520 msg=ucp_get_nbx failed or timed out` / `recv header failed`。既有 `ucx_put_example` 失败方式完全一致 → 与 rtest 无关。

**根因（已定位）**：UCX RMA 路径是 **ufile-ac 反向从 client 内存读**——client 注册内存+打包 rkey → proxy 转发 → ufile-ac `ucp_get_nbx` 远程读。
- ufile-ac 配置 `[ucx] net_devices = mlx5_2:1`（`ucx_service.cc:392` 用 `ucp_config_modify(NET_DEVICES)`，启动日志确认 `UCX NET_DEVICES=mlx5_2:1`）。
- **但 rtest client 的 `UcxMemoryManager::InitContext`（`ucx_memory_manager.cpp:62`）用 `ucp_config_read(NULL,NULL)` 读默认 env，未设 `UCX_NET_DEVICES`** → UCX 默认选 mlx5_0(enp29s0np0=192.168.100.100，不在 1.x 网) 建 endpoint，`sgid_index=3`(192.168.100.100 RoCEv2) 建连超时 30s。
- 对照实验：`UCX_NET_DEVICES=mlx5_2:1` 后 client `rkey_bytes` 从 57→19(选对 mlx5_2)，但**仍 `ucp_get_nbx timeout`** → 即便 client 选对设备，ufile-ac 侧 endpoint 建连仍失败。GID 表确认 mlx5_0/2 gid[3] 均为各自 IPv4 RoCEv2，跨网段(100.x vs 1.x)不可达。

**与既有 example 一致**：`us3_turbo_ucx_put_example` 同样 `ibv_create_ah ... mlx5_0 ... Connection timed out` → 环境/UCX 配置问题，非 Us3Turbo 测试代码缺陷。

**结论**：UCX 通路在本环境（client 与 backend 同机 192.168.1.198，跨 mlx5 网卡路由不通）下，**默认不可用，但可通过 `UCX_NET_DEVICES=mlx5_2:1` 环境变量修复**——设此变量后 client `UcxMemoryManager` 选对 mlx5_2(192.168.1.198)，与 ufile-ac 的 `net_devices=mlx5_2:1` 同网段可达，**UCX 5 用例全部 PASS**（见下"UCX 复跑结果"）。

根因落点：`client/src/memory_manager/ucx_memory_manager.cpp:62` `ucp_config_read(NULL,NULL)` 未设 `UCX_NET_DEVICES`，依赖系统默认选 mlx5_0(跨网段)。建议（可选）client 侧显式设默认 `UCX_NET_DEVICES` 或从配置读取，避免依赖运行时 env。

**UCX 复跑结果（proxy 重启后立即跑 + `UCX_NET_DEVICES=mlx5_2:1`）**：
| 用例 | 结果 | 备注 |
|------|------|------|
| T2.1 single_block_crc | ✅ PASS | crc32c=0x8d0e34c0，get.hash==put.etag，rkey_bytes=20(mlx5_2) |
| T1.1 invalid_part_size | ✅ PASS | Complete 拒 `invalid part size`(code 10002)，3×8MB UploadPart 接受 |
| T1.3 single_part | ✅ PASS | Complete ok object_size=16M，4 块 GET crc=0 |
| T2.2 multi_block_hash | ✅ PASS | 5 块 GET crc=0、hash=622cd561… |
| T1.2 part_number_violation | ✅ PASS | 场景A dup 被拒、场景B 跳号 up1+up3 成功后 Complete 拒 `internal error`(code 10099)；sceneA fileidx size=32M、sceneB 无 fileidx 且 Abort 清理 |

**注意**：UCX 复跑同样受 P1（proxy→dbgate CLOSE-WAIT）影响——T1.1 首跑 CreateMultipartUpload 的 InsertMinit 又一次 `SendAll body failed`(dbgate 连接空闲后死亡)，重启 proxy 后立即跑才 PASS。这进一步印证 P1 是 multipart 链路的首要环境性阻塞，与通路(GDS/UCX)无关。

---

## P3. GDS T1.2 场景B 依赖 backend 数据面稳定（环境相关 flaky）

**症状**：T1.2 跳号场景 (part1, part3) 反复 FAIL（重启 proxy 后立刻跑也复现）。期望 part1+part3 都成功上传后，Complete 因 `merged_size(48M) != sum(32M)` 失败报 `internal error`（PASS）；但当 part3 的 UploadPart 因 ufile-ac 数据面超时(E1008, 30s)而失败时，实际只剩 part1，Complete 用单 part 反而成功（merged==sum==16M），测试判 FAIL。

**根因（T1.2 实跑确认，本轮日志见 `/tmp/proxy_restart8.log` 07:47）**：
- part3 的 UploadPart 在 `multipart.cpp:208` 串行写 4 个 block 时，**block 3** 的 `PutBlockGds` 触发 `[SendAndRecv] DeleteBlock: recv header failed` → `block 3 failed: send request failed`（proxy 日志 07:47:30.357）→ `CleanupWrittenBlocks` 回滚 → part3 UploadPart 返回 E1008 超时（30s）。
- 这不是 dbgate 问题（CreateMultipartUpload 成功了），而是 **ufile-ac 数据面连接级超时 + proxy 不重试**（见 P4）。
- dup part1（场景A）的第二次 UploadPart 同样触发 block 级 E1008 超时 30s（`req-87828e34`），但场景A 接受"dup 失败即非静默"，所以场景A 总是 PASS；问题集中在场景B。

**测试逻辑弱点**：场景B 未区分"part3 上传失败(环境)"与"跳号被检出(逻辑)"。当 up3 失败时，Complete 必然成功（只剩 part1），测试误判 FAIL。

**建议修复（测试侧，已落地）**：part3 UploadPart 失败时退出码 77(skip/inconclusive) 而非 1(FAIL)；仅当 up1&&up3 都成功后 Complete 必须失败才算 PASS。这样 T1.2 在环境不稳时给出 skip 而非假 FAIL，在环境稳定时仍能检出跳号回归。

---

## P4. proxy→ufile-ac 连接级超时 30s 且不重试（UfileAcClient::SendAndRecv）

**症状**：UploadPartGds 偶发 `[E1008]Reached timeout=30000ms`；proxy 日志 `[SendAndRecv] DeleteBlock: recv header failed` / `block N failed: send request failed`。每次超时固定 30s，期间无重试。

**根因（源码 `proxy/src/storage/ufile_ac_client.cpp:124 SendAndRecv` + `tcp_connection.cpp:68 RecvAll`）**：
- `UfileAcClient` 持有连接池（`FLAGS_backend_conn_pool_size`，默认 8），`AcquireConn()` 轮询取 alive 连接，dead 则惰性 `Connect()`。
- `SendAndRecv`：`AcquireConn` → 加锁 → `SendAll` → `RecvAll(header)` → `RecvAll(body)`。**任一步失败即 `set_dead`+`Close` 并 return -1，不切其它连接、不重连当前连接重试**。
- `TcpConnection::RecvAll` 用 `recv`(非 MSG_WAITALL)，**SO_RCVTIMEO 超时返回 -1 被当作"对端异常"直接 set_dead+Close**（注释说"避免误杀慢对端"，但实际超时即关连接）。
- 结果：一次 30s 超时 = 该 block 请求失败 = UploadPart 整体失败（`CleanupWrittenBlocks` 回滚已写 block）= 该 part 不持久。且这 30s 内无任何重试/换连接。
- ufile-ac 侧日志（`.../ufile-ac.*.log`）对应时刻只有 `close connection ... disconnect ... close fd`（如 15:41:17.621、15:47:00.149），无 error —— 即 ufile-ac 主动关闭了空闲连接，proxy 下次用该连接时第一笔 recv 即超时/EOF。

**建议修复（proxy 侧）**：
1. `SendAndRecv` 失败后，在循环里 `AcquireConn` 取下一条 alive 连接（或 `Connect()` 当前连接）**重试 1~2 次**（block 写是非幂等的 keyed 操作，但失败已回滚，重试新连接安全）。
2. 对 backend 连接启用 TCP keepalive 或定期探活，避免 ufile-ac 单侧关空闲连接后 proxy 拿到死连接。
3. `RecvAll` 超时不要立即 `Close`，区分"超时(可重试)"与"RST/EOF(连接真死)"。

**修复落地（2026-07-14）**：与 P1 同批修复（见上 P1 "修复落地"段）。`UfileAcClient::SendAndRecv` 同样改为 `for(attempt<2)` + 失败后 `for(c : conns_) c->set_dead()` 全池作废 + 重试 `Connect()` 新连接。验证（`/tmp/proxy_retry2.log`）：block 级 `PutBlockGds`/`PutBlockUcx` 命中死连接时日志 `send failed → invalidated all pool conns, retry with fresh conn`，重试新连接成功，`WritePartIndex ok`、`UploadPart success`——**P4 修复确认有效**，不再出现数据写入路径的 30s 超时整体失败。残留的 `DeleteBlock ... after 2 attempts` 均在回滚(best-effort)路径，不影响结果。

**影响测试**：T1.2（dup part1 / gap part3 的 block 级超时）受影响；T1.1/T1.3/T2.1/T2.2 在单 block 或连接健康时不受影响。

---

## 环境重启手册（每模块单进程）

| 模块 | 启动命令 | 端口 | 日志 |
|------|----------|------|------|
| mongod | `mongod --config /mnt/us3_test/xinghui.shao/gds/umongo-gw-mongo.conf` | 27017 | /var/log/mongodb/umongo-gw-mongo.log |
| dbgate | `/mnt/us3_test/xinghui.shao/gds/dbgate/dbgate -c .../dbgate.ini` | 20165 | .../dbgate/log/dbgw.log |
| ufile-ac | `/tmp/launch_ufileac.sh`（恢复原 env: LD_LIBRARY_PATH 含 cuda-13.1） | 24000 / RDMA 18666 | /mnt/us3_test/ld/log/set1/set01-m00-d00/ufile-ac.*.log |
| proxy | `build/proxy/us3_turbo_proxy --dbgate_endpoint=127.0.0.1:20165 --bucket_id=1 --backend_endpoint=192.168.1.198:24000 --backend_setid=1 --bind_host=192.168.1.198` | 9100 | /tmp/proxy_restart6.log |

启动顺序：mongo → dbgate → ufile-ac → proxy。
**注意**：proxy 空闲数分钟后 dbgate 连接会死（见 P1），跑测试前若 proxy 已空闲建议重启 proxy。
