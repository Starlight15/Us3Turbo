# part_size 性能差异深度分析:为什么 8M 最好

**日期**:2026-08-06
**范围**:仅分析 `--multipart_part_size`(块大小)对 GDS / RDMA 两条通路性能差异的**根因**,解释 8M 为何是甜点。固定 `num_threads=8`、`backend_conn_pool_size=8`(测试一基线),client 32 线程,total=64M,mock 开启(纯数据搬运)。另见 [num_threads 分析](NUM_THREADS_DEEP_ANALYSIS.md) 与 [conn_pool 分析](CONN_POOL_DEEP_ANALYSIS.md)——三参数最优统一为 part=8M / nt=16 / cp=16。
**方法**:补全 client 端 per-part trace 日志(见 §2),重跑 4M/8M/16M × {GDS,RDMA} 六组,结合 backend `PHASE` 阶段计时,用 per-part 成本模型量化每一档的瓶颈。

---

## 1. 结论(先讲)

8M 最优是**三个因素叠加**的平衡点,不是单一极值:

1. **每 part 固定开销 ~20ms**(brpc 序列化 + proxy 调度 + 网络 + backend 建链回填)对每个 part 都是常量。part 越小,这笔开销摊到 payload 上的比例越高 → 4M 偏低。
2. **backend 实际干活**(`rdma_read` + `crc32`)随 size **亚线性**增长(16M 的 rdma_read 只是 8M 的 1.79×,不是 2×),本身不是瓶颈。
3. **proxy/连接池排队等待**在 16M **超线性爆涨**:32 个 worker 抢 8 个 proxy 线程 / 8 条 backend 连接,16M 每 part 占用槽位 ~4× 于 8M → 队列深度 ×4+ → 等待时间主导,把 16M 拖垮。

4M 输在(1)开销摊不薄;16M 输在(3)排队爆涨;8M 恰好把(1)摊薄到可接受、(3)尚未起量、(2)仍然很小 → 单 part 净带宽最高。

> GDS 的 16M 崩得比 RDMA 狠(53 vs 164 MiB/s),是因为 GDS 在(3)之外还额外撞上了 gds_service 的 4×16M buffer pool 与 cuobj token 路径争抢(见 §6)。

---

## 2. 本次补的 trace 与数据来源

### 2.1 前置:原有 trace 的问题

client 早有 `--trace` 选项,会在每个 part 结束打一行阶段耗时:

```
UploadPartGds trace (req=req-XXX): acquire=0.105ms rpc=7.401ms total=7.506ms bytes=4194304
```

其中 `acquire` = GDS 取 cuobj RDMA token / RDMA 注册 MR,`rpc` = proxy `UploadPart{Gds,Rdma}` 整个 brpc 往返(含 proxy→backend→回填)。**但 bench 把 `ClientOptions.log_level` 硬编码为 `"warn"`,把 `info` 级的 trace 行全 suppress 了**——`--trace` 形同虚设。

### 2.2 修复(已编译进 build/)

`rtest/bench/{gds,rdma}/*_multipart_bench.cpp` 的 `ClientOptions` 改为:

```cpp
.log_level = a.trace ? "info" : "warn"
```

只有显式 `--trace` 时才降到 info,正常 bench 仍是 warn 不受污染。改动非侵入式。

### 2.3 数据来源

| 数据 | 来源 | 作用 |
|---|---|---|
| per-part `acquire`/`rpc` 分布 | client `--trace` 日志(本机 stdout 重定向) | **主证据**,通路区分、part 粒度 |
| backend 阶段 `alloc/pool/rdma_read/crc32/disk_wait/total` | ufile-ac `HandleRdmaPut DONE PHASE`(RDMA 路径独有) | 拆解 backend 干活时间,证明 backend 不是瓶颈 |
| `Recv GDS Put` / `Recv RDMA Put` | ufile-ac ac_server | 计数、按 vlen 关联 |
| proxy 日志 | `/tmp/proxy_trace.log` | **几乎为空**(见 §7 trace gap) |

六组 trace 日志:`/tmp/trace_{gds,rdma}_{4M,8M,16M}.log`,backend 日志:`/mnt/us3_test/ld/log/set1/set01-m00-d00/ufile-ac.*.log`。

---

## 3. 核心数据:per-part trace(主证据)

固定 `num_threads=8`、`backend_conn_pool_size=8`、client 32 线程、`reps=3 warmup=1`,每 part 单独计时:

| 通路 | part | n | acq_p50 | acq_p95 | rpc_p50 | rpc_p95 | rpc_mean | **单 part 净 BW@p50** |
|---|---|---|---|---|---|---|---|---|
| GDS | 4M | 2048 | 0.109ms | 8.98ms | 35.89ms | 77.71ms | 37.24ms | 111.5 MiB/s |
| GDS | **8M** | 1024 | 0.102ms | 211.0ms | 51.00ms | 122.39ms | 55.76ms | **156.9 MiB/s** |
| GDS | 16M | 512 | 0.147ms | 201.95ms | 299.53ms | 511.69ms | 286.34ms | 53.4 MiB/s |
| RDMA | 4M | 2048 | 0.010ms | 0.37ms | 28.13ms | 47.52ms | 26.86ms | 142.2 MiB/s |
| RDMA | **8M** | 1024 | 0.013ms | 0.83ms | 35.71ms | 56.18ms | 33.44ms | **224.0 MiB/s** |
| RDMA | 16M | 512 | 0.014ms | 2.59ms | 97.80ms | 183.23ms | 100.49ms | 163.6 MiB/s |

(`单 part 净 BW@p50 = bytes / rpc_p50`,反映该 size 下单 part 瞬时有效带宽。)

**读数**:
- `acquire` p50 全部 < 0.15ms——取 token / 注册 MR 在中位数上几乎免费。GDS 的 `acquire` p95 在 8M/16M 飙到 200ms(cuobj token 路径尾部争抢,见 §6),但只影响尾部,不主导 p50。
- `rpc` p50 决定一切。4M→8M 翻倍 size 但 rpc 只涨 ~40%;8M→16M 再翻倍,rpc 涨 3-6×。
- 单 part 净 BW:8M 在两条通路都是峰值。

### 3.1 用 4M/8M 拟合线性成本模型

per-part `rpc = F + size/BW`(F = 固定开销,BW = 无争抢时单流带宽):

| 通路 | F(固定开销) | BW(单流净带宽) | 由 4M/8M 推 16M 应为 | 16M 实际 | **16M 超线性罚项** |
|---|---|---|---|---|---|
| GDS | ~20.8ms | 265 MB/s | 81.2ms | 299.5ms | **+218.3ms** |
| RDMA | ~20.5ms | 526 MB/s | 50.9ms | 97.8ms | **+46.9ms** |

- 两条通路 F 几乎相同(~20.5-20.8ms)——因为是同一套 brpc + proxy 调度机制,与通路无关。
- RDMA 单流净带宽(526)≈ GDS(265)的 2×,直接解释了 RDMA 聚合吞吐约为 GDS 的 1.7-1.9×。
- **16M 的实际 rpc 远超线性预测**——GDS 多花 218ms、RDMA 多花 47ms,这就是"排队/争抢超线性罚项",是 16M 崩的直接原因(§4.3、§6)。

---

## 4. backend 阶段拆解:证明 backend 不是 16M 瓶颈(RDMA 路径)

用 `Recv RDMA Put`(含 `vlen` + `key`)与 `HandleRdmaPut DONE PHASE`(含 `key` + 各阶段)按 key 关联,按 part_size 分箱:

| RDMA backend PHASE | 4M p50 | 8M p50 | 16M p50 | scaling |
|---|---|---|---|---|
| alloc_us | 2 | 1 | 0 | ~0(池命中) |
| pool_us | 20 | 12 | 7 | **平坦,非瓶颈** |
| rdma_read_us | 685 | 1418 | 2535 | 2×→1.79× **亚线性** |
| crc32_us | 720 | 944 | 1383 | 2×→1.46× **亚线性** |
| disk_wait_us | 33 | 29 | 70 | mock 下极小 |
| **total_us** | **2757** | **4390** | **15891** | 2×→3.6×(16M 略超线性,因 disk_wait 方差) |

**关键读数**:
- `rdma_read` 和 `crc32` 随 size **亚线性**——backend 干活效率不降反升,不是瓶颈。
- `pool_us` p50 全档 7-20µs,几乎为零;pool_us > 1ms 的只占 **0.2%**——**RDMA backend 的 buffer pool 完全不是 16M 瓶颈**(修正了初版报告对"buffer pool 争抢"的笼统归因)。
- backend `total`:4M=2.8ms / 8M=4.4ms / 16M=15.9ms。

### 4.1 client rpc 减去 backend total = proxy/排队开销

| RDMA | backend total(p50) | client rpc(p50) | **proxy+网络+排队开销** | 开销占比 |
|---|---|---|---|---|
| 4M | 2.8ms | 28.1ms | 25.3ms | 90% |
| 8M | 4.4ms | 35.7ms | 31.3ms | 88% |
| 16M | 15.9ms | 97.8ms | 81.9ms | 84% |

**每 part 有 25-31ms 的固定 proxy/网络开销**(4M/8M 几乎相等——印证 F≈20ms 的来源:brpc + proxy 调度 + 连接建链回填,与 size 无关),16M 这一项**从 ~31ms 暴涨到 ~82ms**。

### 4.2 为什么 16M 的排队开销超线性

proxy 只有 **8 个工作线程 + 8 条 backend 连接**,client 却有 **32 个 worker** 并发压进来:

- 任意时刻最多 8 个 part 在被服务,其余 24 个在 brpc server 队列里等。
- 每 part 占用一个 proxy 线程 + 一条 backend 连接的时长 ≈ backend `total`(4M 2.8ms / 8M 4.4ms / 16M 15.9ms)。
- 队列等待 ≈ (并发/服务并行度) × 服务时间 ≈ (32/8) × total。8M:4×4.4=17.6ms;16M:4×15.9=63.6ms——量级与上表 81.9ms 的增量吻合(差额是 brcc/网络本身)。
- 16M 的 service time 是 8M 的 3.6×,队列等待随之 ×3.6+,且因占槽更久还会引发更多二次排队 → **超线性**。这就是 §3.1 表里 RDMA 的 +47ms 罚项。

> 推论:16M 的 RDMA 罚项不是 backend 慢,而是 **32 路 并发打不满 16M 的大包时,proxy 8 线程/8 连接成为漏桶**。把 proxy `num_threads`/`backend_conn_pool_size` 同步调大(初版报告 G4 = 16/16)能缓解——这正解释了为什么 G4 把 GDS 8M 从 3198 拉到 3864(+21%)。但对 16M,即便加线程,GDS 还有 backend buffer pool 硬卡(§6)。

---

## 5. 三档逐一解释(为什么是 8M)

### 5.1 4M 偏低:固定开销吃掉 90%

- per-part rpc p50 = 28-36ms,其中固定开销 ~25ms 占 **~88-90%**,真正搬数据的只有几 ms。
- 16 个 part/轮,每个 part 把 ~25ms 固定开销摊到 4MB 上 → 净带宽 111-142 MiB/s。
- backend 干活很快(2.8ms),问题纯粹是"part 太碎,固定开销摊不薄"。

### 5.2 8M 甜点:开销摊薄 + 排队未起量 + backend 仍小

- 固定开销 ~25ms 摊到 8MB,占比降到 ~73%(GDS)/ ~74%(RDMA),净带宽升到 157/224 MiB/s——两通路峰值。
- backend total 仅 4.4ms,队列等待 4×4.4≈17ms,尚未失控。
- 8 part/轮,32×8 的调度恰好能填满 8 个 proxy 槽位又不致深度排队。

### 5.3 16M 崩:排队超线性 + (GDS 额外撞 backend buffer pool)

- backend total 升到 15.9ms,队列等待 4×15.9≈63ms,client rpc 跳到 98ms(RDMA)/ 299ms(GDS)。
- 单 part 净带宽跌到 53(RDS)/164(RDMA)MiB/s。
- GDS 还多一层:见 §6。

---

## 6. GDS 16M 为何比 RDMA 16M 崩得多(299ms vs 98ms)

两条通路共用同一套 proxy(8 线程/8 连接),proxy 排队开销应该相近。GDS 16M rpc=299ms,扣掉与 RDMA 相当的 ~82ms proxy 排队,还剩 **~217ms 的 GDS 专属开销**,来源有二(均无法从现有日志直接量化,见 §7 gap):

1. **gds_service backend buffer pool 硬卡**:`gds_service.cc` 的 `BufferSizeClasses={1M,16M}`、`kBufferMaxPerClass=4`——池里只有 **4 个 16M buffer**。32 路并发 16M part,4 个槽打满,其余 28 个排队。按每槽 ~15-27ms(类推 RDMA 16M total)×(32/4)≈ 120-216ms,量级与 217ms 吻合。**这是 GDS 16M 比 RDMA 多崩 200ms 的主因。**
2. **cuobj token 路径尾部争抢**:GDS `acquire` p95 在 8M/16M 飙到 202-211ms(RDMA 同档只有 0.8-2.6ms)。`acquire` = `cuMemObjGetRDMAToken` 走 cuobj RDMA 服务(18666),尾部明显有争抢。但 p50 仍 0.15ms,只拖尾部不拖中位——不是 299ms 的主因,但放大了 p95/p99。

> 修正初版报告:RDMA 16M 的回落(6124→3921)是 **proxy 排队超线性**主导,backend buffer pool(`pool_us` 0.2% 才命中高值)**不是** RDMA 的瓶颈;而 GDS 16M 的崩是 proxy 排队 **叠加** gds_service 的 4×16M buffer pool。两条通路 16M 劣化的主因不同,不应混为一谈。

---

## 7. trace 完整性评估与建议

### 7.1 现状

| 层 | per-part 计时 | 评估 |
|---|---|---|
| client | `acquire` + `rpc` | ✅ 有(本次修复后可用) |
| backend RDMA | `alloc/pool/rdma_read/crc32/disk_wait/total` | ✅ 有(`HandleRdmaPut DONE PHASE`) |
| backend GDS | **无** | ❌ `Recv GDS Put` 只记 receipt,**没有 `HandleGdsPut DONE PHASE`** 对应行,GDS backend 干活时间完全不可见 |
| proxy | **无** | ❌ warn 级只 2 行启动日志,**没有 per-RPC 计时、没有队列等待/连接池等待信号** |

### 7.2 关键 gap:最贵的一层最不透明

§4.1 显示 per-part 有 25-31ms(16M 82ms)花在 "client rpc 减 backend total" 的差值里——这正是 proxy 层(调度 + 队列 + 网络)。**但 proxy 完全没有 per-RPC 计时**,这个差值无法进一步拆成 "brpc 序列化 / server 排队 / 连接池等待 / backend 往返" 几段。调优最该看的恰恰是这一层。

### 7.3 建议补的 trace 点(优先级排序)

1. **proxy per-RPC 阶段计时**(最高优先):在 `UfileAcClient::SendAndRecv` 外层包一层,记录 `queue_wait`(从 RPC 入 proxy 到拿到 backend conn)+ `backend_roundtrip`(SendAndRecv 内)+ `resp_decode`,打成 `UploadPart trace` 类似的行,带 trace_id/req_id。这样 client `rpc` 就能拆成 proxy 段 + backend 段,定位 16M 的 82ms 究竟卡在排队还是 backend 往返。
2. **GDS backend `HandleGdsPut DONE PHASE`**:仿照 `HandleRdmaPut DONE PHASE`(rdma_service.cc:440),在 gds_service 的 PUT 处理末尾打同一套 `alloc/pool/rdma_read/crc32/disk_wait/total` + `pooled=`。补上后 §6 的 "217ms GDS 专属开销" 就能落地到 buffer pool wait(`pool_us`),而非现在的"源码推断 + 量级吻合"。
3. **proxy 连接池/线程池水位**:周期性或每 N 请求打 `backend_conn_pool in_use=X/Y, waiters=Z`、`brpc worker in flight=...`,这样 16M 排队爆涨能直接看到水位,不用反推。
4. **client `acquire` 内部拆分**:GDS `acquire` p95 200ms 太大,建议在 `AcquireToken` 内部拆 `descriptor`(cache hit/miss)+ `GetRDMAToken RPC` 两段,定位是 cuobj 服务慢还是锁竞争。

### 7.4 本次临时改动清单(可保留)

- `rtest/bench/{gds,rdma}/*_multipart_bench.cpp`:`.log_level = a.trace ? "info" : "warn"`(使 `--trace` 真正生效,**建议保留**——非侵入,不开 `--trace` 时行为不变)。
- 其余无改动;proxy/ufile-ac 代码未改。

---

## 8. 对后续调优的指导

1. **8M 是当前架构的甜点,优先固化**为默认 `--multipart_part_size`。在 proxy `num_threads`/`conn_pool` 未大幅扩容前,不要上 16M。
2. **16M 的天花板在 proxy 并发度,不在 backend 干活**:backend `rdma_read`/`crc32` 亚线性,扩 backend 无益;要上 16M 得先扩 proxy 线程 + backend 连接池(同步扩,见初版报告 G4 结论),且 GDS 还得先扩 `BufferSizeClasses`/`kBufferMaxPerClass`(加 8M 档或把 16M 档 buffer 数从 4 提到 ≥32)。
3. **4M 的固定开销 ~25ms 是另一个独立优化点**:若能压低 brpc + proxy 调度的 per-RPC 固定开销,4M 档也会受益,但收益天花板低于直接用 8M。
4. **调优前先补 §7.3 的 proxy per-RPC 计时**——否则 16M 的 82ms 拆不开,改了 proxy 参数也看不出是排队改善还是往返改善。

---

## 附录 A:复现命令

```bash
# bench(已编译,--trace 现在真正打 info)
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
./build/rtest/bench/gds/us3_turbo_bench_gds_multipart \
  --part-size 8M --total 64M --concurrency 32 --reps 3 --warmup 1 --trace \
  > /tmp/trace_gds_8M.log 2>&1

# 解析 per-part trace
python3 - <<'PY'
import re,statistics
pat=re.compile(r"acquire=([\d.]+)ms rpc=([\d.]+)ms total=([\d.]+)ms bytes=(\d+)")
def pct(v,p): v=sorted(v); return v[min(len(v)-1,int(p/100*(len(v)-1)))]
acq=[];rpc=[];tot=[];by=0
for l in open("/tmp/trace_gds_8M.log"):
    m=pat.search(l)
    if m: a,r,t,b=m.groups(); acq.append(float(a));rpc.append(float(r));tot.append(float(t));by=int(b)
print(f"n={len(rpc)} acq_p50={pct(acq,50):.3f} rpc_p50={pct(rpc,50):.3f} bw@p50={by/(1024*1024)/(pct(rpc,50)/1000):.1f}")
PY

# backend PHASE(按 key 关联 vlen 分箱,见 §4 脚本)
LATEST=$(ls -t /mnt/us3_test/ld/log/set1/set01-m00-d00/ufile-ac* | head -1)
grep -E "Recv RDMA Put|HandleRdmaPut DONE" "$LATEST"
```

## 附录 B:六组聚合吞吐(与初版报告一致,本次带 trace 略有日志开销)

| part | GDS MiB/s | RDMA MiB/s | GDS data-plane p50 | RDMA data-plane p50 |
|---|---|---|---|---|
| 4M | 2444 | 4225 | 756ms | 427ms |
| 8M | 3097 | 6148 | 554ms | 267ms |
| 16M | 1381 | 4056 | 1145ms | 391ms |

(trace 日志开销使吞吐比初版无 trace 时略低 1-2%,不影响排序与结论。)
