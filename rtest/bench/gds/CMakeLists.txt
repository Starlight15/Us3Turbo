# bench/gds/ — GDS 性能基准（device 显存，链 CUDA）。

rtest_add_target(us3_turbo_bench_gds_put gds_put_bench.cpp NEEDS_CUDA)
rtest_add_target(us3_turbo_bench_gds_multipart gds_multipart_bench.cpp NEEDS_CUDA)
rtest_add_target(us3_turbo_bench_gds_get gds_get_bench.cpp NEEDS_CUDA)
