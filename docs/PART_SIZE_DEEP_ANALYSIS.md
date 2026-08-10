# part_size 调参:为什么 8M 最好

**最近校验**:2026-08-10(mock,`mock_aio_write=1` 两路,`nt=8/cp=8`、`[gds] wt=8`,governor=performance,client 32,64M,reps≥3 warmup1)

---

## 1. 结论(一句话)

**GDS 与 RDMA 都在 8M 达峰**:4M 输在固定开销摊不薄,16M 输在单 part 占用槽位过久(GDS 主 loop backlog 崩、RDMA 32 并发 16M 撞 RNIC/在途上限)。8M 恰好摊薄开销又未触发排队爆涨。

## 2. 数据(08-10 校验,mock)

| part | GDS (MiB/s) | RDMA (MiB/s) | 形状 |
|---|---|---|---|
| 4M | 3096 | 10185 | 偏低(开销摊不薄) |
| **8M** | **3755** | **~10900** | **峰** |
| 16M | 1759(**崩 -53%**) | ~9900(-9%) | 16M 不再最优 |

GDS 16M 灾难性崩(1759 < 4M 3096),RDMA 16M 仅略低于 8M(扁平)。

## 3. 三档原因(精炼)

- **4M 偏低**:每 part 固定开销 ~20ms(brpc 序列化 + proxy 调度 + 网络 + backend 建链回填)是常量;part 越小,这笔开销摊到 payload 比例越高 → 净带宽低。
- **8M 甜点**:固定开销摊薄到可接受;backend 干活(rdma_read + crc)随 size 亚线性(16M rdma_read 仅 8M 的 1.79×);排队尚未起量 → 单 part 净带宽最高。
- **16M 崩**:32 并发 × 16M = 512MB 在途,单 part 占槽位 ~4× 于 8M → 队列深度×4。GDS 主 EventLoop 串行 backlog 爆涨 → 16M 崩;RDMA 因 mock 跳 memcpy(commit 4fdd47a)免了主 loop memcpy 罚项,故只略降(~9%)而非崩。

## 4. 历史勘误(08-07 "RDMA 16M 反转为最优"已取代)

08-07 复测曾记 RDMA 16M=7761 > 8M=6516("16M 反转为最优")。那是**低吞吐 regime**(governor=powersave,~6500 量级)下的结论:mock memcpy skip 消除 16M 主 loop 罚项后,大 part 摊薄占优。08-10 在 governor=performance(~10000 量级)下,32 并发 16M 在途撞 RNIC/在途上限,16M 反而略低于 8M。**8M 为峰更稳健**(两 regime 下 8M 均不输,16M 仅在低吞吐 regime 占优)。

## 5. 环境与复现

```bash
# mock 两路(纯搬运,无落盘);proxy nt=8/cp=8,backend [gds] wt=8
cd /mnt/us3_test/xinghui.shao/gds/Us3Turbo
# 扫不同 part 时须改 proxy --multipart_part_size 与 bench --part-size 一致,重启 proxy
for p in 4194304 8388608 16777216; do
  sed -i "s/--multipart_part_size=.*/--multipart_part_size=$p/" proxy/conf/proxy.flags
  pkill -x us3_turbo_proxy; sleep 2
  nohup ./build/proxy/us3_turbo_proxy --flagfile=proxy/conf/proxy.flags > /tmp/p.log 2>&1 &
  sleep 3
  m=$((p/1048576))
  ./build/rtest/bench/gds/us3_turbo_bench_gds_multipart  --part-size ${m}M --total 64M --concurrency 32 --reps 3 --warmup 1
  ./build/rtest/bench/rdma/us3_turbo_bench_rdma_multipart --part-size ${m}M --total 64M --concurrency 32 --reps 3 --warmup 1
done
```

> 注:绝对值随 governor(performance≈2× powersave,因 mock 是 CPU-bound)、nt/cp 变化;**结论(8M 峰)在两 regime 下均成立**,绝对值不可跨 regime 直接比。
