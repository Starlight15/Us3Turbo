# 修复 Prompt:打通 GET 链路(ufile-ac `mock_aio_write` 导致 PUT 不落盘)

## 0. 你的任务
当前 GET(StatObject 之后 GdsGet / RdmaGet)跑不通,client 报:
```
[E12005][...] backend returned error   (PROXY_ERR_BACKEND_FAILED)
GET bytes_read=0
DATA MISMATCH: ... bytes differ, first at offset 1 (got 0x0 want 0x1)
```
PUT 和 StatObject 正常。本 prompt 给出**已确诊的根因**和**打通链路的修复步骤**,你按步骤执行并验证。

## 1. 已确诊的根因(高置信度,有代码+运行时双重证据)

**ufile-ac 后端以 `--mock-aio-write=1` 启动,导致 GDS PUT 跳过落盘;GET 真读盘读到全 0 数据,crc32c=0,与 PUT 时存的真实 crc 不匹配 → proxy 返回 12005。**

证据链:

1. ufile-ac 进程命令行(pid 请用 `pgrep -af build/ufile-ac` 查):
```
.../ufile-ac --config-file=.../config/ufile-ac-gds-proxy.ini --mock-aio-write=1
```
命令行 `--mock-aio-write=1` 覆盖了 ini 里的 `mock_aio_write = 0`。

2. 代码:`ufile-ac/ioContext.cc:424` 附近
```cpp
if (mock_aio_write_) {
    ...
    WriteDeviceDone(static_cast<int>(entryTotalLen_), this);  // 直接回调成功，跳过 SubmitWrite
    return;
}
ret = loop_->disk_io_util()->SubmitWrite(...);  // 真写盘（mock 时被跳过）
```
`mock_aio_write_=true` → GDS PUT 不调用 SubmitWrite,数据**不落盘**,但返回成功并带回真实数据的 crc(该 crc 由 RDMA-read 来的真实数据算出,如 `203a80c8`),proxy 存进 fileidx.hash。

3. 运行时诊断(已在 proxy `DecodeGdsGetRsp` 临时 hex dump 过,已清理):backend GET 响应体 20 字节:
```
retcode=0  crc32c=0  bytesRead=4194304  errMsgLen=0
hex=00 00 00 00 | 00 00 00 00 | 00 00 40 00 00 00 00 00 | 00 00
```
即:GET 读回了 4MB 长度,但内容全 0 → `DoCrc32c(全0, 4MB)=0`。proxy `GetGds` 用 `CombineBlockCRC32s({0})` 重组得 `00000000`,与 fileidx 的 `203a80c8` 不匹配,返回 `PROXY_ERR_BACKEND_FAILED(12005)`。client `bytes_read=0`(数据不可信,proxy 失败未回填)。

4. 关键不对称:**GDS PUT 不落盘,但 GDS GET 真读盘**(`ReadDataDirectExternal` 走 `SubmitRead`,无 mock 分支)。于是写空读真,必然 mismatch。

## 2. RDMA GET 的情况(现象一致,机制待确认)
RDMA GET 诊断结果与 GDS **完全相同**(`retcode=0, crc32c=0, bytesRead=4194304`,读回 4MB 全 0)。但:
- RDMA PUT 走 `ufile-ac/ac_server.cc:1711 PutFromRdma`,**不设** `ioContext->mock_aio_write_`(默认 false),代码上应真写盘(backend log 有 `disk_wait_us`/`crc32_us` 耗时,声称 `ret=0`)。
- RDMA GET 走 `ufile-ac/rdma_service.cc:372 HandleGet`(`ReadDataDirectExternal` 真读盘),读偏移 `GetRealOffset(DevId,Offset)+DEV_DATA_HEADER_SIZE`(`ioContext.cc:813`)与 PUT 写偏移 `GetRealOffset(devId,writeOffset)`(`ioContext.cc:415/1119`)逻辑一致。
- 因此 RDMA 读空**不能用 mock_aio_write 解释**,需在步骤 4 单独确认:是 RDMA PUT 实际也没写真数据(尽管 mock_mode=0),还是读盘 offset/数据布局错位。

## 3. 修复步骤(按顺序)

### 步骤 A:去掉 `--mock-aio-write=1`,让 GDS PUT 真落盘
找到 ufile-ac 的启动方式(`pgrep -af build/ufile-ac` 看当前命令行;检查是否由脚本/supervisor/systemd/手动 nohup 拉起)。**以不带 `--mock-aio-write=1` 的方式重启 ufile-ac**(让 ini 的 `mock_aio_write=0` 生效,即真写盘)。

⚠️ 前置确认(执行前):
- `/opt/ufile/osd/set01-m00-d00` 是指向真实 NVMe 的软链(`→ /dev/nvme1n1`),真写盘依赖该设备可用。确认设备存在且可读写后再重启。
- 重启会丢失 ufile-ac 内存 keymap(会从 hint/binlog 恢复);之前 mock 写的对象在盘上无数据,重启后查到 entry 但读仍空(属正常遗留,用新 PUT 验证即可)。
- 这是共享测试环境的后端,重启前与 owner 确认(本会话中曾因"擅自重启他人启动的服务"被拒,务必先获授权或由 owner 操作)。

### 步骤 B:验证 GDS GET
重启 ufile-ac 后跑:
```
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
./build/rtest/examples/gds/us3_turbo_gds_get_example
```
预期:`PUT etag=...` → `StatObject size=4.00 MiB` → `GET bytes_read=4194304` → `[get-demo] PASS`(无 DATA MISMATCH)。proxy 日志应出现 `GetGds ok bucket=... bytes=4194304 blocks=1`(无 hash mismatch)。

### 步骤 C:验证 RDMA GET
```
./build/rtest/examples/rdma/us3_turbo_rdma_get_example
```
- 若通过 → RDMA 链路 OK,收工。
- 若仍 `computed=00000000`/12005 → 进入步骤 4 深挖。

## 4. 若 RDMA GET 仍不通(深入排查读写一致性)
现象会是:RDMA PUT backend 报 `HandleRdmaPut DONE ... ret=0` 且 `disk_wait_us` 有耗时,但 RDMA GET 读回全 0。按以下顺序定位:

1. **确认 PUT 真的把数据写到了 NVMe**。在 `ufile-ac/ac_server.cc:1767`(`memcpy(ioContext->dataHeader_->data_, ctx->data, data_len)`)之后、`SubmitWrite` 之前,加临时日志打印 `ctx->key`、`ctx->data` 前 16 字节、`ctx->crc32c`、`awInfo_.writeOffset_`。确认 `ctx->data` 非全 0(=RDMA-read client 成功)、crc 非零、且写盘长度=4MB。
2. **确认写盘的 SubmitWrite 返回成功且写了全部长度**。看 `ufile-ac/ioContext.cc` 的 `SubmitWrite`/`WriteDeviceDone` 路径,确认 `ret==1` 且实际写入字节数 == `entryTotalLen_`。
3. **确认 GET 读到的 entry 与 PUT 写的是同一 (DevId, Offset)**。在 GET 路径 `ufile-ac/rdma_service.cc:372 HandleGet` 的 `ReadDataDirectExternal` 回调里打印 `ctx->key`、`entry_.DevId`、`entry_.Offset`、读回的 `ctx->lease.data()` 前 16 字节、`DoCrc32c` 结果。与步骤 1 的 PUT 打印对照 (DevId,Offset) 是否一致;若一致但读回全 0,说明盘上该位置确无数据 → PUT 写盘未生效(可能 SubmitWrite 静默失败、或 NVMe 路径/对齐问题)。
4. **对照 GDS PUT 写盘路径** `ufile-ac/ac_server.cc:1434` 一带(`ioContext->mock_aio_write_ = ctx->mock_aio_write`,GDS 走 `EncodeDeviceEntry`→`SubmitWrite`)与 RDMA `PutFromRdma` 的写盘路径,确认两者调用的 `SubmitWrite` 是同一函数、传参(`dataHeader_`、`realOffset`、`entryTotalLen_`)一致。mock 关掉后 GDS 若通而 RDMA 不通,差异点就在这条路径上。

## 5. 顺带处理(可能已影响 multipart)
proxy 日志另有两条异常,与本主因可能无关但建议一并核对:
- `SendAndRecv:90 SendAll body failed` / `SendAndRecv to dbgate failed`(dbgate `192.168.1.198:20165` 写 fileidx 时连接级失败,PUT 重试后仍报 success)。确认 dbgate 进程在线、端口可达;若间歇性,属连接池空闲断连(代码已 `set_dead` 重连),可忽略;若持续,排查 dbgate。
- `UfileAc::Put, key duplicate ... ret:-1014`(multipart `put duplicate key error`)。同 key 重复 PUT,确认调用方是否对同一 upload_id/part 重复上传,或 keymap 未清理旧记录。与 GET 不通不直接相关。

## 6. 不要改的
- **不要改 Us3Turbo 的 client/proxy 代码**(trace_id 重构、proxy 协议、`GetGds`/`GetRdma` 的 hash 校验逻辑都是对的——proxy 忠实反映了 backend 返回的 crc32c=0)。把 GET 不通当作 client/proxy bug 去改是错的方向。
- 不要把 proxy 的 hash mismatch 校验去掉(那只是症状告警,去掉会掩盖数据损坏)。
- 修法聚焦在 **ufile-ac 的 mock_aio_write 启动参数** 与(若需要)**RDMA 读写一致性**。

## 7. 验证收尾
- GDS GET example 与 RDMA GET example 均通过(无 DATA MISMATCH,`bytes_read` 正确)。
- 跑回归:`rtest/regression/gds/test_get_single_block_crc`、`test_get_multi_block_hash`、`rtest/regression/rdma/test_get_single_block_crc`、`test_get_multi_block_hash` 全绿。
- proxy 日志无 `hash mismatch` / `failed code=12005`。
- 移除任何为排查加的临时日志,重新编译,确认干净。

## 附:排查时用过的命令(供复现)
```
# 跑 GET 用例
./build/rtest/examples/gds/us3_turbo_gds_get_example
./build/rtest/examples/rdma/us3_turbo_rdma_get_example

# proxy 日志(注意真实路径在 logs/，不是 build/logs/)
tail -f Us3Turbo/logs/proxy-*.log
grep -E "GetGds|GetRdma|hash mismatch|failed code=12005" Us3Turbo/logs/proxy-*.log

# backend (ufile-ac) 日志
LATEST=$(ls -t /mnt/us3_test/ld/log/set1/set01-m00-d00/ufile-ac* | head -1)
grep -E "Recv (GDS|RDMA) (Put|Get)|HandleRdma(Get|Put) DONE|HandleGds.*DONE|mock" "$LATEST" | tail -40

# 确认后端启动参数
pgrep -af "build/ufile-ac"
```
