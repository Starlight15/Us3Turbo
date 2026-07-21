# examples/gds/ — GDS 端到端验证示例（device 显存，链 CUDA）。

rtest_add_target(us3_turbo_gds_put_example gds_put_example.cpp NEEDS_CUDA)
rtest_add_target(us3_turbo_gds_get_example gds_get_example.cpp NEEDS_CUDA)
rtest_add_target(us3_turbo_gds_multipart_example gds_multipart_example.cpp NEEDS_CUDA)
