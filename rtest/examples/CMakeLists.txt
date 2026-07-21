# examples/ — 端到端验证示例（教学价值：演示如何使用 client API）。
#
# 编译产物:
#   build/rtest/examples/us3_turbo_gds_put_example        — GDS 单次 PUT 端到端
#   build/rtest/examples/us3_turbo_gds_get_example        — GDS 单步+分段 PUT 后 GET 读回验证
#   build/rtest/examples/us3_turbo_gds_multipart_example  — GDS 分段上传端到端
#   build/rtest/examples/rdma/us3_turbo_rdma_put_example       — RDMA (libibverbs) 单步 PUT 端到端
#   build/rtest/examples/rdma/us3_turbo_rdma_multipart_example — RDMA (libibverbs) 分段上传端到端

add_subdirectory(gds)
add_subdirectory(rdma)
