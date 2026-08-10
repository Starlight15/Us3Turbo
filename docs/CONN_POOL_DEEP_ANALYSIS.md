# backend_conn_pool_size 调参:cp≥nt,饱和点随吞吐上移

**最近校验**:2026-08-10(mock,`mock_aio_write=1` 两路,part 8M,nt=16 固定,governor=performance,client 32,64M,reps3 warmup1)

---

## 1. 结论(一句话)

**cp ≥ nt**(backend 连接池 ≥ proxy 线程数),且 cp 要给够(≥16)才喂饱 GDS。RDMA 钝感。08-10 高吞吐下两处弱化:cp<nt 不再大跌(扁平)、饱和点从 cp=16 上移到 cp≥32(再大仍略升)。

## 2. 数据(08-10 校验,mock,nt=16)

| cp | cp vs nt | GDS (MiB/s) | RDMA (MiB/s) |
|---|---|---|---|
| 4 | cp<nt (4:1) | 3689 | 10458 |
| 8 | cp<nt (2:1) | 3582 | 10312 |
| 16 | cp=nt | 4197 | 10812 |
| 32 | cp>nt | 4418 | 10932 |

- GDS:cp4≈cp8(~3600,扁平),cp16 跳到 4200,cp32 仍略升(4418)。**cp<16 喂不饱,cp≥16 才进高位**。
- RDMA:全档 10312–10932(±3%),**钝感** ✓。

## 3. 原因(精炼)

- **cp<nt 劣化 = 连接争抢**:nt>cp 时 proxy 线程抢不到 backend 连接,空等 `conn_mutexes_`。08-07(低吞吐)cp4 的 GDS rpc p50 82.4ms vs cp16 的 49.3ms(conn-pool 等待反推 ~13.2ms),坐实。
- **GDS 敏感、RDMA 钝感**:GDS part 重(backend 干活久,conn 占用久)→ 喂不饱时吞吐跌;RDMA part 轻 → 瓶颈在 RNIC,cp 不动。
- **08-10 弱化**:governor=performance 下 backend/proxy 更快 → conn 占用时间占比下降 → cp<nt 的争抢罚项缩(cp4 不再大跌,~3600 而非 2200);同时吞吐更高 → 需更多连接喂饱 → 饱和点从 cp=16 上移(cp32 仍略升)。

## 4. 调优规则

- **cp ≥ nt,且 cp ≥ 16**(GDS 高吞吐下 cp32 仍略优,但 cp16 已进 95% 区,选 16 够用且省连接)。
- RDMA:任意 cp 到硬件天花板,选小省资源。
- 高吞吐下 cp 精确值边际小(cp16 vs cp32 差 +5%);**只别让 cp<nt 严重 under-provision**。

## 5. 历史勘误

08-07 复测记 GDS cp4=2233 << cp8=3080 << cp16≈cp32(3985/3756,"cp<nt 跌、cp=16 饱和")。08-10 同点:cp4≈cp8(~3600,不再大跌)、cp32(4418)略 > cp16(4197,饱和点上移)。低吞吐 regime 的"cp<nt 剧烈劣化 + cp=16 拐点"被高吞吐抹平/上移。**cp≥nt 规则两 regime 下均成立**,仅拐点位置与劣化强度需弱化。

## 6. 复现

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
# mock 两路,part 8M,nt=16 固定,扫 cp = 改 proxy --backend_conn_pool_size
sweep() {
  sed -i "s/--backend_conn_pool_size=.*/--backend_conn_pool_size=$1/" proxy/conf/proxy.flags
  pkill -x us3_turbo_proxy; sleep 2
  nohup ./build/proxy/us3_turbo_proxy --flagfile=proxy/conf/proxy.flags >/tmp/p.log 2>&1 & sleep 3
  ./build/rtest/bench/gds/us3_turbo_bench_gds_multipart  --part-size 8M --total 64M --concurrency 32 --reps 3 --warmup 1
  ./build/rtest/bench/rdma/us3_turbo_bench_rdma_multipart --part-size 8M --total 64M --concurrency 32 --reps 3 --warmup 1
}
sweep 4; sweep 8; sweep 16; sweep 32
```
