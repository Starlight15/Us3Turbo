# num_threads 性能差异深度分析:proxy 线程数的影响

**日期**:2026-08-06
**范围**:仅分析 proxy `--num_threads`(工作线程数)对 GDS / RDMA 两条通路性能的影响与根因。承接 [part_size 分析](PART_SIZE_DEEP_ANALYSIS.md)——那次发现 16M 的罚项是"32 worker 抢 8 proxy 线程 / 8 backend 连接"的排队超线性,`num_threads` 正是控制该队列深度的旋钮。配套 [conn_pool 分析](CONN_POOL_DEEP_ANALYSIS.md)从 cp 方向交叉验证本文的 `nt≈cp` 规则。
**方法**:控制变量(只改 nt),固定 `backend_conn_pool_size=8`、`part_size=8M`(part_size 分析的最优)、client 32 线程、total=64M、mock 开启。扫描 nt ∈ {4,8,16,32}(从 8 倍过排队到 1:1 无排队)。补一个决定性对照点 nt=16/cp=16(cp 与 nt 同步放大)以定位 nt>cp 时劣化的根因。

---

## 1. 结论(先讲)

1. **nt 不应超过 cp(proxy 线程数 ≤ backend 连接池大小)**。nt>cp 时,多出的线程抢不到连接,在 `conn_mutexes_` 上空转,per-part rpc 上涨、吞吐反降。
2. **GDS 对 nt 敏感**(part 重,backend 干活久):cp=8 固定下,nt=4→8→16→32 的吞吐 2953→3097→2802→2582,**峰值在 nt=8(=cp),之后单调下降**。决定性对照:同一 nt=16,cp 8→16 让吞吐 2802→3564(+27%)、per-part rpc 62.5→49.3ms——**证明 nt>cp 时的劣化是 conn-pool 争抢,不是 nt 本身**。
3. **RDMA 对 nt 钝感**(part 轻,瓶颈在 RNIC/NVMe 硬件):nt=4→8→16→32 吞吐 6229→6148→6369→6320,±3% 波动,per-part rpc 全档 33-36ms。
4. **调优规则**:
   - GDS:nt 与 cp **同步放大**(都往 16/32 走,但 nt≤cp),才能在保住并行度的同时不撞 conn 争抢。本轮最优 nt=16/cp=16 → 3564 MiB/s。
   - RDMA:任意 nt 都到硬件天花板(~6300-6500),**选小的(4 或 8)省资源**即可。

> 与初版报告测试二(粗 2×2)的"GDS 两参数必须同步上调"结论一致,但本次用 per-part trace 把根因定位到 **conn-pool 争抢**(nt>cp 时空等连接),并把"GDS 敏感 / RDMA 钝感"的差别归到 **part 轻重**(GDS rpc ~50ms vs RDMA ~35ms,backend 干活 GDS 更久 → conn 占用更久 → 更易争抢)。

---

## 2. 方法与数据

### 2.1 控制变量

| 参数 | 值 | 说明 |
|---|---|---|
| `backend_conn_pool_size` | 8(主扫描固定) | 隔离 nt 效应 |
| `multipart_part_size` | 8M | part_size 分析的最优 |
| client concurrency | 32 | 固定 |
| total | 64M | 固定 |
| reps / warmup | 3 / 1 | 每配置每通路 |
| `--trace` | 开 | per-part `acquire`/`rpc` 日志(bench `log_level` 已修复为 `trace?"info":"warn"`) |

- 主扫描:nt ∈ {4,8,16,32},cp=8 固定,每档跑 GDS+RDMA。
- 决定性对照:nt=16/cp=16(cp 同步放大),定位 nt>cp 劣化根因。
- nt=8/cp=8 点直接复用 part_size 分析的 `trace_{gds,rdma}_8M.log`(配置完全一致)。

### 2.2 数据来源

| 数据 | 来源 |
|---|---|
| per-part `acquire`/`rpc` | client `--trace` 日志 `/tmp/trace_nt{4,8,16,32}_{gds,rdma}.log`、`/tmp/trace_nt16cp16_{gds,rdma}.log` |
| backend 阶段 | ufile-ac `HandleRdmaPut DONE PHASE`(RDMA 独有,GDS 无,见 [part_size 分析 §7](PART_SIZE_DEEP_ANALYSIS.md#7-trace-完整性评估与建议) gap) |
| proxy | 仍无 per-RPC 计时(同上 gap) |

---

## 3. 结果

### 3.1 聚合吞吐

| nt | cp | GDS MiB/s | GDS data-plane p50 | RDMA MiB/s | RDMA data-plane p50 |
|---|---|---|---|---|---|
| 4 | 8 | 2953 | 601ms | 6229 | 265ms |
| **8** | 8 | **3097** | 554ms | 6148 | 267ms |
| 16 | 8 | 2802 | 646ms | 6369 | 261ms |
| 32 | 8 | 2582 | 764ms | 6320 | 272ms |
| 16 | **16** | **3564** | 510ms | 6533 | 258ms |

- GDS(cp=8):**峰值在 nt=8(=cp)**,nt>cp 后单调下降;nt=32 跌到 2582(比峰值 -17%)。
- RDMA(cp=8):全档 6148-6369,±3%,钝感。
- 对照 nt=16/cp=16:GDS 跳到 3564(比 nt=16/cp=8 的 2802 **+27%**,比 nt=8/cp=8 峰值 +15%);RDMA 6533(与 cp=8 同档持平)。

### 3.2 per-part trace(主证据)

| 通路 | nt | cp | acq_p50 | acq_p95 | **rpc_p50** | rpc_p95 | per-part BW@p50 |
|---|---|---|---|---|---|---|---|
| GDS | 4 | 8 | 0.111ms | 214ms | 55.0ms | 124ms | 145.4 MiB/s |
| GDS | **8** | 8 | 0.102ms | 211ms | **51.0ms** | 122ms | **156.9 MiB/s** |
| GDS | 16 | 8 | 0.143ms | 161ms | 62.5ms | 125ms | 127.9 MiB/s |
| GDS | 32 | 8 | 0.162ms | 225ms | 90.7ms | 110ms | 88.2 MiB/s |
| GDS | 16 | **16** | 0.090ms | 213ms | **49.3ms** | 100ms | **162.2 MiB/s** |
| RDMA | 4 | 8 | 0.012ms | 0.87ms | 35.7ms | 56.6ms | 224.1 MiB/s |
| RDMA | 8 | 8 | 0.013ms | 0.83ms | 35.7ms | 56.2ms | 224.0 MiB/s |
| RDMA | 16 | 8 | 0.011ms | 1.02ms | 33.5ms | 55.0ms | 238.7 MiB/s |
| RDMA | 32 | 8 | 0.013ms | 0.96ms | 34.4ms | 50.0ms | 232.5 MiB/s |
| RDMA | 16 | 16 | 0.013ms | 0.94ms | 33.4ms | 52.3ms | 239.5 MiB/s |

**读数**:
- **GDS rpc_p50**:cp=8 下 nt=4→8→16→32 = 55→51→62.5→90.7ms,U 形(峰值 nt=8)。nt=16/cp=16 跌回 **49.3ms**(全表最低)。
- **RDMA rpc_p50**:全档 33-36ms,**完全平**,含 cp 放大也只 33.4。
- `acquire` p50 全部 < 0.2ms(取 token 不主导);GDS `acquire` p95 ~160-225ms(cuobj 路径尾部争抢,与 [part_size 分析 §6](PART_SIZE_DEEP_ANALYSIS.md#6-gds-16m-为何比-rdma-16m-崩得多299ms-vs-98ms) 同源),但 p50 不动,只拖尾部。

---

## 4. 机制分析

### 4.1 GDS:nt>cp 时劣化 = conn-pool 争抢(决定性证据)

`UfileAcClient::AcquireConn`(`proxy/src/storage/ufile_ac_client.cpp:26`)的实现:池有 cp 条连接,每条带独立 `conn_mutexes_[idx]`;`SendAndRecv` 取到连接后 `lock_guard` 整个请求周期持锁。

- **nt ≤ cp(nt=4/8, cp=8)**:每条 in-flight 请求独占一条连接,无锁等待。rpc 由 backend 干活 + 固定开销决定 ≈ 51ms。nt=8 时不排队(8 槽喂 8 线程),吞吐 3097。
- **nt > cp(nt=16/32, cp=8)**:16/32 个 bthread 抢 8 条连接,多出的线程在 `conn_mutexes_[idx]` 上阻塞空等。多出的线程**不增加吞吐**(连接数才是漏桶),只增加:
  1. 连接锁争抢 + AcquireConn 的 `for k in size` 扫描(`ufile_ac_client.cpp:29`)开销;
  2. 线程切换、conn pool mutex 热点;
  3. 一旦某连接空闲关闭,`set_dead` 把全池标死(`ufile_ac_client.cpp:190`)引发级联重连。
  → per-part rpc 从 51 涨到 62.5(nt16)再到 90.7ms(nt32),**吞吐反降**。

**决定性对照(同一 nt=16)**:

| nt=16 | cp=8 | cp=16 |
|---|---|---|
| GDS rpc_p50 | 62.5ms | **49.3ms** |
| GDS 吞吐 | 2802 | **3564** |
| GDS per-part BW | 127.9 | **162.2** |

**唯一变量是 cp**。cp 8→16 把 rpc 砍掉 13.2ms、吞吐 +27%——这 13.2ms 就是 nt=16/cp=8 时的 **conn-pool 等待**。根因坐实:nt>cp 的劣化来自连接池争抢,而非 nt 本身或 backend。

### 4.2 为什么 nt=8/cp=8 是 cp=8 下的 GDS 峰值,而不是 nt=4

nt=4→8,cp=8:吞吐 2953→3097(+5%),rpc 55→51ms。nt 从 4 翻到 8,**首次填满 cp=8 的 8 条连接并行度**(nt<cp 时部分连接闲置),backend 并行度从 4 升到 8。收益小是因为 32 个 worker / 8 槽已是 4 倍过载,backend 并行度早在 nt=4 就接近吃满,nt=4→8 只是把"连接偶尔闲置"补齐。再往上(nt>cp)就只剩争抢、无并行度可补 → 反降。

### 4.3 RDMA:为何对 nt 钝感

RDMA per-part rpc 全档 33-36ms,吞吐全档 6148-6533。三层原因:

1. **part 轻**:RDMA 8M 的 backend `total` 仅 4.4ms(`HandleRdmaPut DONE PHASE`,见 [part_size 分析 §4](PART_SIZE_DEEP_ANALYSIS.md#4-backend-阶段拆解证明-backend-不是-16m-瓶颈rdma-路径)),rpc 35ms 里 backend 只占 4.4ms,其余 ~30ms 是 brpc + proxy 调度 + 网络固定开销。连接占用短(35ms vs GDS 51ms),turnover 快,conn-pool 不积压。
2. **硬件先行饱和**:32 worker 即便只 nt=4 也能把 RNIC/NVMe 喂到天花板(6229 MiB/s),nt 再大也无硬件可补 → 平。
3. **conn-pool 不争抢**:part 轻 + turnover 快,即便 nt>cp(如 nt=32/cp=8),conn 锁等待时间也短到被 backend 方差淹没,rpc 看不出上涨(34.4ms,反而比 nt=8 的 35.7 还略低,在误差内)。

> GDS rpc ~51ms(part 重,conn 占用久)对 conn 争抢敏感;RDMA rpc ~35ms(part 轻,conn 占用短)对 conn 争抢钝感——这是两通路对 nt 响应不同的**根因差异**,与 [part_size 分析](PART_SIZE_DEEP_ANALYSIS.md) 里 RDMA 单流净带宽 2× GDS 的结论同源(GDS backend 路径更重)。

### 4.4 backend 侧是否随 nt 变化?

RDMA backend `total`(8M)= 4.4ms(已测),且 RDMA rpc 跨 nt 全平 → backend 侧不随 nt 变。GDS backend 无 PHASE(无法直测),但 GDS rpc 随 nt 单调变 → GDS 的变化全在 proxy/conn 层,与 backend 无关。**即:调 nt 影响的是 proxy 排队与 conn 争抢,不触及 backend 干活效率。**

---

## 5. 最优 num_threads

| 通路 | 推荐 nt | 理由 |
|---|---|---|
| GDS | **= cp,且与 cp 同步放大**(本轮 nt=16/cp=16 最优 3564) | nt<cp 浪费连接;nt>cp conn 争抢;nt=cp 且都≥client 并发/2 才能填满并行度 |
| RDMA | **4 或 8 即可**(任意值都到 ~6300-6500) | 硬件天花板,nt 不影响;选小省 bthread/内存 |

**通用规则**:`num_threads ≤ backend_conn_pool_size`。违反此规则(线程多于连接)必导致 conn-pool 争抢、吞吐下降,GDS 尤甚。

**是否继续放大(nt=cp=32)?** client 只有 32 worker,nt=32/cp=32 达 1:1:1 无排队,理论可能再涨 GDS,但:
- 32 worker 已能喂满,nt=16/cp=16 已把 GDS 拉到 3564(接近 GDS backend 单流带宽 × 并行的实际上限);
- 继续放大需 cp=32(更多长连接、更多内存),收益边际、成本上升;
- 本轮未测 nt=32/cp=32,列为后续(见附录)。

---

## 6. trace 完整性:本轮仍靠"推断"定位 conn 争抢

本轮的关键量化——nt=16/cp=8 时 GDS rpc 里有 **13.2ms 是 conn-pool 等待**(由 cp 8→16 的差值反推)——是**反推**得出,因为 proxy 没有 per-RPC 计时:

- `UfileAcClient::AcquireConn` / `SendAndRecv` 内部没有 `queue_wait`(等连接的耗时)和 `backend_roundtrip`(拿到连接后的发收)分段打点。
- 所以"rpc 上涨 = conn 争抢"是**唯一变量排除法**(cp 是唯一变量)坐实,而非直接测得。

**建议(与 [part_size 分析 §7.3](PART_SIZE_DEEP_ANALYSIS.md#73-建议补的-trace-点优先级排序) 一致,本轮再次印证其必要性)**:
1. 在 `SendAndRecv` 加 `t_acquire_conn` / `t_lock_held` / `t_backend_rsp` 三段计时,打成 `proxy trace (req=...): conn_wait=Xms backend=Yms total=Zms`。补上后:
   - nt>cp 劣化可直接看到 `conn_wait` 上涨(而非现在靠 cp 差值反推);
   - 可定位 nt=32/cp=8 时 90.7ms 里多少是 conn_wait、多少是 backend 排队、多少是锁本身;
   - 调 cp/nt 时能直接看 conn_wait 是否清零,而非看吞吐间接判断。
2. 周期打 `backend_conn_pool in_use=X/Y waiters=Z` 水位,nt=32/cp=8 时应能看到 waiters>0。

---

## 7. 对调优的指导

1. **立即固化的默认**:`num_threads = backend_conn_pool_size`,且都设 16(本轮 GDS 最优 3564,RDMA 也到 6533)。这是兼顾两通路的安全配置。
2. **别犯 nt>cp**:测试中 nt=32/cp=8 让 GDS 从 3097 跌到 2582(-17%),是典型的"线程开太多反而更慢"。
3. **RDMA 不需要大 nt**:若某部署只跑 RDMA,nt=4/8 足够,省资源。
4. **GDS 要继续涨吞吐**:瓶颈已从 proxy(本轮 16/16 解决了排队)移到 backend GDS 路径(gds_service 的 4×16M buffer pool,见 [part_size 分析 §6](PART_SIZE_DEEP_ANALYSIS.md#6-gds-16m-为何比-rdma-16m-崩得多299ms-vs-98ms))。下一步该调 backend buffer pool,而非继续加 proxy 线程。
5. **调 cp/nt 前先补 §6 的 proxy per-RPC 计时**:否则 conn 争抢只能靠控制变量反推,改了参数也看不到 conn_wait 直接曲线。

---

## 附录 A:完整数据表(cp=8 主扫描 + cp=16 对照)

见 §3.1、§3.2。

## 附录 B:复现命令

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
# 改 nt(cp 固定 8,part 8M)
sed -i 's/^--num_threads=.*/--num_threads=16/' proxy/conf/proxy.flags
sed -i 's/^--backend_conn_pool_size=.*/--backend_conn_pool_size=8/' proxy/conf/proxy.flags
sed -i 's/^--multipart_part_size=.*/--multipart_part_size=8388608/' proxy/conf/proxy.flags
kill $(pgrep -x us3_turbo_proxy); sleep 2
nohup ./build/proxy/us3_turbo_proxy --flagfile=proxy/conf/proxy.flags > /tmp/proxy_nt.log 2>&1 &
sleep 4
./build/rtest/bench/gds/us3_turbo_bench_gds_multipart \
  --part-size 8M --total 64M --concurrency 32 --reps 3 --warmup 1 --trace \
  > /tmp/trace_nt16_gds.log 2>&1
# per-part 解析见 PART_SIZE_DEEP_ANALYSIS.md 附录 A 脚本(acquire=/rpc= 正则)
```

## 附录 C:后续待测

1. **nt=cp=32**(1:1:1,无排队上限)是否再涨 GDS?client 32 worker 是理论上限。
2. **nt=16/cp=32**(cp>nt,连接富余)是否消除 GDS conn 争抢、与 nt=16/cp=16 差多少——确认 cp 的边际收益拐点。
3. 补 proxy per-RPC 计时后,直接画 conn_wait vs nt 曲线(现在是反推)。
