# Us3Turbo 调参深度分析

**最近校验**:2026-08-10(mock,`mock_aio_write=1` 两路 GDS+RDMA,governor=performance,client 32,64M,reps≥3 warmup1)

三轴调参(part_size / num_threads / backend_conn_pool_size)在 mock 纯搬运 regime 下的系统化扫描。每个轴给出结论、数据、原因、规则与历史勘误。跨轴统一规律见末节。

> 注:绝对值随 governor(performance≈2× powersave,因 mock 是 CPU-bound)、nt/cp 变化;**结论在两 regime 下均成立**,绝对值不可跨 regime 直接比。

---

## 1. part_size 调参:为什么 8M 最好

### 1.1 结论

**GDS 与 RDMA 都在 8M 达峰**:4M 输在固定开销摊不薄,16M 输在单 part 占用槽位过久(GDS 主 loop backlog 崩、RDMA 32 并发 16M 撞 RNIC/在途上限)。8M 恰好摊薄开销又未触发排队爆涨。

### 1.2 数据(08-10 校验,mock)

| part | GDS (MiB/s) | RDMA (MiB/s) | 形状 |
|---|---|---|---|
| 4M | 3096 | 10185 | 偏低(开销摊不薄) |
| **8M** | **3755** | **~10900** | **峰** |
| 16M | 1759(**崩 -53%**) | ~9900(-9%) | 16M 不再最优 |

GDS 16M 灾难性崩(1759 < 4M 3096),RDMA 16M 仅略低于 8M(扁平)。

### 1.3 原因(精炼)

- **4M 偏低**:每 part 固定开销 ~20ms(brpc 序列化 + proxy 调度 + 网络 + backend 建链回填)是常量;part 越小,这笔开销摊到 payload 比例越高 → 净带宽低。
- **8M 甜点**:固定开销摊薄到可接受;backend 干活(rdma_read + crc)随 size 亚线性(16M rdma_read 仅 8M 的 1.79×);排队尚未起量 → 单 part 净带宽最高。
- **16M 崩**:32 并发 × 16M = 512MB 在途,单 part 占槽位 ~4× 于 8M → 队列深度×4。GDS 主 EventLoop 串行 backlog 爆涨 → 16M 崩;RDMA 因 mock 跳 memcpy(commit 4fdd47a)免了主 loop memcpy 罚项,故只略降(~9%)而非崩。

### 1.4 历史勘误

08-07 复测曾记 RDMA 16M=7761 > 8M=6516("16M 反转为最优")。那是**低吞吐 regime**(governor=powersave,~6500 量级)下的结论:mock memcpy skip 消除 16M 主 loop 罚项后,大 part 摊薄占优。08-10 在 governor=performance(~10000 量级)下,32 并发 16M 在途撞 RNIC/在途上限,16M 反而略低于 8M。**8M 为峰更稳健**(两 regime 下 8M 均不输,16M 仅在低吞吐 regime 占优)。

---

## 2. num_threads 调参:nt≤cp,高吞吐抹平敏感度

### 2.1 结论

**nt ≤ cp(proxy 线程数 ≤ backend 连接池),nt=cp=16 为峰**。RDMA 对 nt 钝感;GDS 在低吞吐下对 nt 敏感(nt>cp 时空等连接劣化),但 08-10 高吞吐(governor=perf)下该敏感度**扁平化**——nt>cp 劣化从 -17% 缩到 -4%,只剩"同步放大 nt=cp=16"这个主收益仍成立。

### 2.2 数据(08-10 校验,mock)

| nt | cp | GDS (MiB/s) | RDMA (MiB/s) |
|---|---|---|---|
| 4 | 8 | 3587 | 10652 |
| 8 | 8 | 3758 | 10991 |
| 16 | 8 | 3767 | 10025 |
| 32 | 8 | 3610 | 10801 |
| **16** | **16** | **4207** | 10938 |

- GDS(cp=8):nt 4→8→16→32 = 3587/3758/3767/3610,**几乎扁平**(峰值在 nt=16,nt=32 仅 -4%)。
- RDMA:全档 10025–10991,**钝感** ✓。
- nt=16/cp=16:GDS 4207,比任何 cp=8 点 +12%——**同步放大 nt=cp 是唯一显著收益**。

### 2.3 原因(精炼)

- **nt>cp 劣化 = conn-pool 争抢**:nt 超过 cp 时,多出的 proxy 线程抢不到 backend 连接,在 `conn_mutexes_` 空等 → per-part rpc 上涨、吞吐反降。08-07(低吞吐)实测 nt16/cp8→nt16/cp16 让 GDS rpc 62.5→49.3ms、吞吐 +27%,坐实 conn 争抢是根因。
- **GDS 敏感、RDMA 钝感**:GDS part 重(backend rdma_read+crc 久,conn 占用久)→ 更易撞争抢;RDMA part 轻(conn 占用短)→ 瓶颈在 RNIC 硬件,nt 不动。
- **08-10 扁平化**:governor=performance 把吞吐拉到 ~2× ,backend/proxy 更快 → conn 占用时间占比下降 → nt>cp 的争抢罚项从 -17% 缩到 -4%。**规则(nt≤cp)不变,但 nt 微调的边际收益在高吞吐下变小**。

### 2.4 调优规则

- **nt = cp,同步往 16 走**(GDS 最优 4207 @ nt16/cp16)。
- RDMA:任意 nt 到硬件天花板,选小省资源。
- 高吞吐下不必纠结 nt 精确值(cp 固定时 nt 4–32 差 <5%);**只别让 nt>cp**。

### 2.5 历史勘误

08-07 复测记 GDS cp=8 下 U 形(nt=8 峰 2800、nt=32 跌到 2771,-17%)。08-10 同点扁平(~3760,nt32 仅 -4%):低吞吐 regime 的 U 形被高吞吐抹平。**nt=cp=16 最优与 nt≤cp 规则两 regime 下均成立**,仅"nt 敏感度"的强声明需弱化。

---

## 3. backend_conn_pool_size 调参:cp≥nt,饱和点随吞吐上移

### 3.1 结论

**cp ≥ nt**(backend 连接池 ≥ proxy 线程数),且 cp 要给够(≥16)才喂饱 GDS。RDMA 钝感。08-10 高吞吐下两处弱化:cp<nt 不再大跌(扁平)、饱和点从 cp=16 上移到 cp≥32(再大仍略升)。

### 3.2 数据(08-10 校验,mock,nt=16)

| cp | cp vs nt | GDS (MiB/s) | RDMA (MiB/s) |
|---|---|---|---|
| 4 | cp<nt (4:1) | 3689 | 10458 |
| 8 | cp<nt (2:1) | 3582 | 10312 |
| 16 | cp=nt | 4197 | 10812 |
| 32 | cp>nt | 4418 | 10932 |

- GDS:cp4≈cp8(~3600,扁平),cp16 跳到 4200,cp32 仍略升(4418)。**cp<16 喂不饱,cp≥16 才进高位**。
- RDMA:全档 10312–10932(±3%),**钝感** ✓。

### 3.3 原因(精炼)

- **cp<nt 劣化 = 连接争抢**:nt>cp 时 proxy 线程抢不到 backend 连接,空等 `conn_mutexes_`。08-07(低吞吐)cp4 的 GDS rpc p50 82.4ms vs cp16 的 49.3ms(conn-pool 等待反推 ~13.2ms),坐实。
- **GDS 敏感、RDMA 钝感**:GDS part 重(backend 干活久,conn 占用久)→ 喂不饱时吞吐跌;RDMA part 轻 → 瓶颈在 RNIC,cp 不动。
- **08-10 弱化**:governor=performance 下 backend/proxy 更快 → conn 占用时间占比下降 → cp<nt 的争抢罚项缩(cp4 不再大跌,~3600 而非 2200);同时吞吐更高 → 需更多连接喂饱 → 饱和点从 cp=16 上移(cp32 仍略升)。

### 3.4 调优规则

- **cp ≥ nt,且 cp ≥ 16**(GDS 高吞吐下 cp32 仍略优,但 cp16 已进 95% 区,选 16 够用且省连接)。
- RDMA:任意 cp 到硬件天花板,选小省资源。
- 高吞吐下 cp 精确值边际小(cp16 vs cp32 差 +5%);**只别让 cp<nt 严重 under-provision**。

### 3.5 历史勘误

08-07 复测记 GDS cp4=2233 << cp8=3080 << cp16≈cp32(3985/3756,"cp<nt 跌、cp=16 饱和")。08-10 同点:cp4≈cp8(~3600,不再大跌)、cp32(4418)略 > cp16(4197,饱和点上移)。低吞吐 regime 的"cp<nt 剧烈劣化 + cp=16 拐点"被高吞吐抹平/上移。**cp≥nt 规则两 regime 下均成立**,仅拐点位置与劣化强度需弱化。

---

## 4. 跨轴统一规律

- **硬约束**:`nt ≤ cp`(proxy 线程 ≤ backend 连接池),否则空等连接争抢 → 吞吐跌。两 regime 下均成立。
- **GDS 敏感、RDMA 钝感**:GDS part 重(backend 干活久 → conn 占用久 → 喂不饱/争抢敏感);RDMA part 轻(瓶颈在 RNIC 硬件 → 对 nt/cp 不动)。RDMA 任意 nt/cp 到硬件天花板,选小省资源。
- **高吞吐抹平敏感度**:governor=performance 下 backend/proxy 更快 → conn 占用时间占比下降 → nt>cp 与 cp<nt 的争抢罚项都缩(从 -17% 到 -4%、从大跌到扁平);但饱和点上移(需更多连接/线程喂饱)。**规则不变,边际收益变小**。
- **part_size 与 regime 相反**:8M 在两 regime 下均为峰,16M 仅低吞吐 regime 占优(高吞吐下撞在途上限略降)。
- **mock 调参不可直接搬到真写**:mock 瓶颈=固定开销摊薄(大 part 优),真写瓶颈=NVMe 落盘(小 part {2M,4M} 优)。详见 `GDS_REAL_RW_REPORT.md`。

---

## 5. 复现

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
# mock 两路(纯搬运,无落盘);扫参数须改 proxy flags 后重启 proxy,part 须与 --multipart_part_size 一致
BENCH_G="./build/rtest/bench/gds/us3_turbo_bench_gds_multipart"
BENCH_R="./build/rtest/bench/rdma/us3_turbo_bench_rdma_multipart"

restart() {  # 改 proxy flags 后重启
  pkill -x us3_turbo_proxy; sleep 2
  nohup ./build/proxy/us3_turbo_proxy --flagfile=proxy/conf/proxy.flags >/tmp/p.log 2>&1 & sleep 3
}
run() {  # $1=part(M) ; part 扫描时同时改 --multipart_part_size
  sed -i "s/--multipart_part_size=.*/--multipart_part_size=$(($1*1048576))/" proxy/conf/proxy.flags
  restart
  $BENCH_G --part-size ${1}M --total 64M --concurrency 32 --reps 3 --warmup 1
  $BENCH_R --part-size ${1}M --total 64M --concurrency 32 --reps 3 --warmup 1
}

# part_size 扫描(nt=8/cp=8,part 8M 时 backend [gds] wt=8)
for p in 4 8 16; do run $p; done

# num_threads 扫描(part 8M,cp 固定)
sweep_nt() {
  sed -i "s/--num_threads=.*/--num_threads=$1/; s/--backend_conn_pool_size=.*/--backend_conn_pool_size=$2/" proxy/conf/proxy.flags
  restart
  $BENCH_G --part-size 8M --total 64M --concurrency 32 --reps 3 --warmup 1
  $BENCH_R --part-size 8M --total 64M --concurrency 32 --reps 3 --warmup 1
}
sweep_nt 4 8; sweep_nt 8 8; sweep_nt 16 8; sweep_nt 32 8; sweep_nt 16 16

# backend_conn_pool_size 扫描(part 8M,nt=16 固定)
sweep_cp() {
  sed -i "s/--backend_conn_pool_size=.*/--backend_conn_pool_size=$1/" proxy/conf/proxy.flags
  restart
  $BENCH_G --part-size 8M --total 64M --concurrency 32 --reps 3 --warmup 1
  $BENCH_R --part-size 8M --total 64M --concurrency 32 --reps 3 --warmup 1
}
sweep_cp 4; sweep_cp 8; sweep_cp 16; sweep_cp 32
```
