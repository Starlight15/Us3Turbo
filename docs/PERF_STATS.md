# 性能打点规范（PERF_STATS）

三层（client / proxy / backend）统一的性能打点，µs 粒度、`key=val` 格式、可 `grep` + python 聚合。默认关，零开销；性能测试时显式开启，跑一次即可拉全链路各段分布，无需临时改代码。

## 统一输出格式

```
[perf/<layer>] req=<rid> op=<op> [key=<block_key>] <stage>_us=<n> ... total_us=<n> [bytes=<n>]
```

- **µs 粒度**（`duration_cast<microseconds>`）；proxy/backend 段常 <1ms，ms 会丢精度。
- **key=val 空格分隔**，行首 `[perf/<layer>]` 标签：grep `\[perf/` 拉全链路。
- backend 沿用其既有行首（`[rdma-dispatch]` / `[rdma-read]` / `[gds-timing]`），同风格、同 grep 方式，视作 backend 层。

### 各层字段

**client**（`[perf/client]`，`client/src/common/trace.h::TraceLatency`）
```
[perf/client] req=<rid> op=UploadPartRdma acquire_us=<MR token 获取> rpc_us=<client→proxy 整段 RPC> total_us=<start→rpc> bytes=<part>
```
- `acquire_us` = client 注册/取 MR token（client 侧已缓存，稳态 ~µs）。
- `rpc_us` = client→proxy 往返（含 proxy 内部 + backend RPC + 网络）——proxy 侧会再拆。

**proxy per-RPC**（`[perf/proxy]`，`proxy/src/service/multipart.cpp`）
```
[perf/proxy] req=<rid> op=UploadPartRdma key=<block_key> validate_us=<校验+prep> backend_us=<PutBlock*> index_us=<WritePartIndex> total_us=<全段> bytes=<part>
```
- `backend_us` = 调 `PutBlockRdma`/`PutBlockGds` 的总时（包络下一条 SendAndRecv 三段）。
- `key=<block_key>` = 跨层关联锚点，即 backend `[rdma-dispatch] key:` 的值。

**proxy SendAndRecv**（`[perf/proxy]`，`proxy/src/storage/ufile_ac_client.cpp::SendAndRecv`）
```
[perf/proxy] op=<PutBlockRdma|...>:sendrecv acquire_conn_us=<取连接+per-conn锁> send_us=<TCP SendAll> recv_us=<等backend处理+回包> attempt=<重试次>
```
- `acquire_conn_us` ≈ 0 = backend conn pool 无争抢；涨 = 连接池/锁瓶颈。
- `send_us` ≈ µs（TCP 发）。
- `recv_us` ≈ backend 处理 + 网络 ≈ proxy `backend_us` —— 定位"proxy 慢还是 backend 慢"的关键：`recv_us` 大=backend 慢；`acquire_conn_us` 大=proxy 内部序列化慢。

**backend**（既有，`ufile-ac/rdma_service.cc` + `ioContext.cc` + `hintfile.cc`，env `US3_GDS_PUT_TIMING`）
```
[rdma-dispatch] key:<k> post_us=<主loop backlog> exec_us=<PutFromRdma工作> notify_us=<cv唤醒> exec_breakdown malloc_us/memcpy_us/keysmap_us/alloc_us/done_cb_us=<...>
[rdma-read]    key:<k> regmr_us=<ibv_reg_mr pin> postread_us=<ibv_post_send> read_us=<PollOne 真read> rdma_read_us=<合计>
[gds-timing]   aio_write/hint_write/worker_queue_wait/...（GDS 路径）
```

## 开关

| 层 | 开关 | 默认 | 备注 |
|---|---|---|---|
| client | bench `--trace`（设 `ClientOptions::latency_trace`，并自动把 client log_level 设 info） | off | bench 子命令 |
| proxy | `--enable_perf_stats=true`（gflag，`proxy/src/common/flags.cpp`） | false | **须配合 `--log_level=info`**（proxy 默认 warn，info 行会被吞） |
| backend | `US3_GDS_PUT_TIMING=1` env + `[gds]`/`[rdma] mock_aio_write`/`US3_MOCK_HINT_WRITE` | off | 后端启动 env |

### 一键全开（proxy+backend+bench）

```bash
# proxy
nohup ./build/proxy/us3_turbo_proxy --flagfile=proxy/conf/proxy.flags \
    --enable_perf_stats=true --log_level=info > /tmp/proxy_perf.log 2>&1 &

# backend
US3_GDS_PUT_TIMING=1 US3_MOCK_HINT_WRITE=1 nohup ./build/ufile-ac \
    --config-file=config/ufile-ac-gds-proxy.ini > /tmp/uac.log 2>&1 &
# （US3_MOCK_HINT_WRITE / [rdma] mock_aio_write 仅纯数据搬运测试用；真读写不加）

# bench（--trace 开 client 侧）
./build/rtest/bench/rdma/us3_turbo_bench_rdma_multipart \
    --proxy 192.168.1.198:9100 --part-size 8M --total 64M \
    --concurrency 16 --reps 20 --warmup 0 --trace
```

关掉开关（默认）后无 `[perf/` / `[rdma-*` 行；perf 代码 `if (FLAGS_...)` 门控，开销可忽略（仅若干 `steady_clock::now()`，~百 ns/请求）。

## 跨层关联

`req` 与 `key` 串三层：

```
client [perf/client] req=<rid>                  # client 侧请求 id
  ↕ 同 rid
proxy  [perf/proxy]   req=<rid> key=<block_key> # block_key = obj_id + "_" + (part-1)
  ↕ 同 key
backend [rdma-dispatch] key:<block_key>          # backend 用 key
```

定位时：拿一个慢请求的 `req`，grep proxy 日志找 `req=<rid>` → 读 `key=` → grep backend 日志 `key:<block_key>` → 看 `[rdma-read] regmr_us` / `[rdma-dispatch] exec_us` 哪段大。批量看分布用下面的聚合。

## 聚合脚本（按 `<stage>_us=` 抽字段算 avg/p50/p95/max）

```bash
# 例：proxy per-RPC 各段分布
grep '\[perf/proxy\] req=' /tmp/proxy_perf.log | python3 -c '
import sys,re
from collections import defaultdict
v=defaultdict(list)
for l in sys.stdin:
    for k in ["validate_us","backend_us","index_us","total_us"]:
        m=re.search(k+r"=([0-9]+)",l)
        if m: v[k].append(int(m.group(1)))
def st(a):
    a=sorted(a);n=len(a);return f"n={n} avg={sum(a)/n:.0f} p50={a[n//2]} p95={a[int(n*0.95)]} max={a[-1]}"
for k in v: print(f"{k:12s} {st(v[k])}")
'
```

backend `[rdma-*]` 同法（字段名换 `regmr_us`/`read_us`/`exec_us`/`post_us`/`notify_us`）。client `[perf/client]` 同法（`acquire_us`/`rpc_us`/`total_us`）。

## 一致性 sanity

- proxy `backend_us` ≈ SendAndRecv `recv_us` ≈ backend `total_us`（+ 网络 RTT）。
- `acquire_conn_us` ≈ 0（backend conn pool 不争抢）；若涨 → `backend_conn_pool_size` 不够或锁热。
- client `rpc_us` ≈ proxy `total_us`（+ client↔proxy 网络）。

## 实现位置一览

| 改动 | 文件 |
|---|---|
| client 输出 µs + `[perf/client]` | `client/src/common/trace.h` |
| proxy `ElapsedUs`/`UsSince` | `proxy/src/common/utils.h` |
| `enable_perf_stats` gflag | `proxy/src/common/flags.{h,cpp}`、`proxy/conf/proxy.flags` |
| SendAndRecv 三段计时 | `proxy/src/storage/ufile_ac_client.cpp` |
| multipart per-RPC 计时 | `proxy/src/service/multipart.cpp`（UploadPartGds/Rdma） |
| backend（既有，未改） | `ufile-ac/rdma_service.cc`、`ioContext.cc`、`hintfile.cc` |

## 后续

- GET 路径（`GetBlockRdma`）未加 perf（同模式可后补，PUT multipart 是 bench 主路径）。
- 后端 MR pool 优化（regmr 7ms 瓶颈）是下一个独立任务，见本轮结论。
