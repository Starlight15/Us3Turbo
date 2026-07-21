# examples/rdma/ — RDMA (libibverbs) 端到端示例（host 内存，无 CUDA 依赖）。

rtest_add_target(us3_turbo_rdma_put_example rdma_put_example.cpp NEEDS_RDMA)
