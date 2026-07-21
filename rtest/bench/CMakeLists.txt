# rtest/bench — 性能基准工具。
# 子目录按通路组织：gds/（device 显存，链 CUDA）/ rdma/（host 内存，链 ibverbs）。
add_subdirectory(gds)
add_subdirectory(rdma)
