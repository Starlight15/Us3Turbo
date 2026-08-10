# bench 工具 warmup 统计修复

**修复日期**:2026-08-06

## 背景

跑真实读写单步测试时发现 bench 工具两个 warmup 相关统计 bug。这两个 bug **不影响
multipart bench**(即三个 `*_DEEP_ANALYSIS.md` 用的
bench),其 warmup round 返回值被丢弃、不进 stats;只影响单步 PUT/GET bench。

## Bug 1:单步 PUT warmup 污染吞吐

`rtest/bench/{gds,rdma}/*_put_bench.cpp` 的 `do_put` lambda 同时服务 warmup 与测量:

```cpp
auto do_put = [&](const std::string& key) {
  ... PUT ...
  if (ok) { stats.rounds.push_back(...); ++stats.ok; stats.bytes += a.size; }  // warmup 也走这
  ...
};
for (i in 0..warmup) do_put(warmup_key);   // barrier 前,不计 wall 分母
sync.arrive_and_wait();
while (...) do_put(measure_key);            // 计 wall 分母
```

`throughput = stats.bytes / wall`,warmup 在 barrier 前(不计入 wall)但 bytes 被累加
→ 吞吐随 warmup 虚高。实测 RDMA 单步 4M count=40 conc=8:warmup=2 报 3390、
warmup=4 报 5888、warmup=0 真值 2475。

## Bug 2:GET bench warmup 命中假 key

`rtest/bench/rdma/rdma_get_bench.cpp` warmup 用 key `key_prefix-warmup-{wid}-{i}`,
这些 key 从未 PUT(main 只串行播种 `key_prefix-0..count-1`),warmup GET 全 fail,
产生 `fail=N`(N=warmup×concurrency)假失败;且同 Bug 1,warmup bytes 污染吞吐。

## 修复

**单步 PUT**:拆出 `put_one`(只 PUT 返回 ok,warmup 调它不记账),测量阶段才
计时 + push round + 累加 bytes/ok:

```cpp
auto put_one = [&](const std::string& key) -> bool { ... return PUT ok; };
for (i in 0..warmup) (void)put_one(warmup_key);   // 不记账
sync.arrive_and_wait();
while (...) {
  t0; ok = put_one(key); t1;
  if (ok) { rounds.push_back({..t1-t0..}); ++ok; bytes += size; }
}
```

**GET**:拆 `get_one`,warmup 改为命中已播种 key(`i % count`)而非 `-warmup-` 假 key,
不记账、不计时、不产生假 fail:

```cpp
auto get_one = [&](const std::string& key) -> bool { ... return GET ok; };
for (i in 0..warmup) (void)get_one(key_prefix + "-" + to_string(i % count));  // 命中真 key
```

## 验证

修复后 `ok` 严格等于 `count`(warmup 未混入分子),`fail=0`(GET 无假失败)。
单步 PUT warmup=0 与 warmup=2 吞吐差仅来自真实 NVMe 预热(同数据块重写、热路径),
而非字节虚增。multipart bench 数值与修复前一致(旁证其本不受影响)。

## 影响

- 单步 PUT/GET bench:修复后可放心用 warmup=2 预热。
- multipart bench 与历史调参报告:无影响,warmup round 本就不进 stats。
