# bench/rdma/ — RDMA (libibverbs) 性能基准（host 内存，链 ibverbs/rdmacm）。

rtest_add_target(us3_turbo_bench_rdma_put rdma_put_bench.cpp NEEDS_RDMA)
rtest_add_target(us3_turbo_bench_rdma_multipart rdma_multipart_bench.cpp NEEDS_RDMA)
