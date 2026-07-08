// block_storage.cpp — 阶段三：所有方法已 inline 到 block_storage.h（透传壳）。
//
// 原 brpc 数据面实现（PutPartGds/Ucx、SplitToBlocks、Aggregate、
// CallBackendPutBlockGds/Ucx、brpc channel/stub）已删除，本文件保留为空以维持
// CMakeLists.txt 源列表不变（doc 步骤 4 方案 A：保留空实现，避免改构建配置）。

#include "proxy/src/storage/block_storage.h"

namespace us3_turbo::proxy {

// 无非 inline 成员实现。

}  // namespace us3_turbo::proxy
