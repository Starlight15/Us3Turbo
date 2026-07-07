#pragma once

// flags.h — proxy 控制面命令行参数（gflags）集中定义。
//
// 定义集中在 common/flags.cpp；本头仅 DECLARE 供各翻译单元引用。
// gflags 标志为全局符号，故不放入 us3_turbo::proxy 命名空间（与既往 main.cpp
// 顶置 DEFINE 一致），调用方直接写 FLAGS_xxx 即可。
//
// 参数语义：
// - proxy_port                  控制面 brpc 监听端口
// - bind_host                   brpc 监听地址
// - num_threads                 brpc worker 线程数
// - backend_endpoint            后端数据面地址（GdsPut/UcxPut/PutBlock 转发目标）
// - backend_timeout_ms          proxy→backend 转发超时（覆盖 GdsPut/UcxPut/PutBlock）
// - upload_ttl_ms               multipart 会话 TTL（ms），超时由清理线程删除
// - upload_ttl_scan_interval_ms TTL 清理扫描周期（ms）
// - backend_block_size_bytes    multipart part 切 block 大小（bytes，block_storage）
// - log_level                   app 日志级别：debug/info/warn/error
// - log_max_size_mb             app 日志单文件上限（MB），满则滚动
// - log_max_files               app 日志滚动保留文件数

#include <gflags/gflags.h>

DECLARE_int32(proxy_port);
DECLARE_string(bind_host);
DECLARE_int32(num_threads);
DECLARE_string(backend_endpoint);
DECLARE_int32(backend_timeout_ms);
DECLARE_int64(upload_ttl_ms);
DECLARE_int64(upload_ttl_scan_interval_ms);
DECLARE_uint64(backend_block_size_bytes);
DECLARE_string(log_level);
DECLARE_int32(log_max_size_mb);
DECLARE_int32(log_max_files);
