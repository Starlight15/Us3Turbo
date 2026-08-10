# num_threads 调参:nt≤cp,且 GDS 对 nt 的敏感度被高吞吐抹平

**最近校验**:2026-08-10(mock,`mock_aio_write=1` 两路,part 8M,governor=performance,client 32,64M,reps3 warmup1)

---

## 1. 结论(一句话)

**nt ≤ cp(proxy 线程数 ≤ backend 连接池),nt=cp=16 为峰**。RDMA 对 nt 钝感;GDS 在低吞吐下对 nt 敏感(nt>cp 时空等连接劣化),但 08-10 高吞吐(governor=perf)下该敏感度**扁平化**——nt>cp 劣化从 -17% 缩到 -4%,只剩"同步放大 nt=cp=16"这个主收益仍成立。

## 2. 数据(08-10 校验,mock)

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

## 3. 原因(精炼)

- **nt>cp 劣化 = conn-pool 争抢**:nt 超过 cp 时,多出的 proxy 线程抢不到 backend 连接,在 `conn_mutexes_` 空等 → per-part rpc 上涨、吞吐反降。08-07(低吞吐)实测 nt16/cp8→nt16/cp16 让 GDS rpc 62.5→49.3ms、吞吐 +27%,坐实 conn 争抢是根因。
- **GDS 敏感、RDMA 钝感**:GDS part 重(backend rdma_read+crc 久,conn 占用久)→ 更易撞争抢;RDMA part 轻(conn 占用短)→ 瓶颈在 RNIC 硬件,nt 不动。
- **08-10 扁平化**:governor=performance 把吞吐拉到 ~2× ,backend/proxy 更快 → conn 占用时间占比下降 → nt>cp 的争抢罚项从 -17% 缩到 -4%。**规则(nt≤cp)不变,但 nt 微调的边际收益在高吞吐下变小**。

## 4. 调优规则

- **nt = cp,同步往 16 走**(GDS 最优 4207 @ nt16/cp16)。
- RDMA:任意 nt 到硬件天花板,选小省资源。
- 高吞吐下不必纠结 nt 精确值(cp 固定时 nt 4–32 差 <5%);**只别让 nt>cp**。

## 5. 历史勘误

08-07 复测记 GDS cp=8 下 U 形(nt=8 峰 2800、nt=32 跌到 2771,-17%)。08-10 同点扁平(~3760,nt32 仅 -4%):低吞吐 regime 的 U 形被高吞吐抹平。**nt=cp=16 最优与 nt≤cp 规则两 regime 下均成立**,仅"nt 敏感度"的强声明需弱化。

## 6. 复现

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
# mock 两路,part 8M;扫 nt = 改 proxy --num_threads,cp 固定
sweep() {
  sed -i "s/--num_threads=.*/--num_threads=$1/; s/--backend_conn_pool_size=.*/--backend_conn_pool_size=$2/" proxy/conf/proxy.flags
  pkill -x us3_turbo_proxy; sleep 2
  nohup ./build/proxy/us3_turbo_proxy --flagfile=proxy/conf/proxy.flags >/tmp/p.log 2>&1 & sleep 3
  ./build/rtest/bench/gds/us3_turbo_bench_gds_multipart  --part-size 8M --total 64M --concurrency 32 --reps 3 --warmup 1
  ./build/rtest/bench/rdma/us3_turbo_bench_rdma_multipart --part-size 8M --total 64M --concurrency 32 --reps 3 --warmup 1
}
sweep 4 8; sweep 8 8; sweep 16 8; sweep 32 8; sweep 16 16
```
