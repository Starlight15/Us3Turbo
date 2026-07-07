#pragma once

// flags.h — proxy 控制面命令行参数（gflags）集中定义。
//
// 定义集中在 common/flags.cpp；本头仅 DECLARE 供各翻译单元引用。
// gflags 标志为全局符号，故不放入 us3_turbo::proxy 命名空间（与既往 main.cpp
// 顶置 DEFINE 一致），调用方直接写 FLAGS_xxx 即可。
//
// 参数语义：
// - proxy_port         控制面 brpc 监听端口
// - bind_host          brpc 监听地址
// - num_threads        brpc worker 线程数
// - backend_endpoint   后端数据面地址（GdsPut/UcxPut/PutBlock 转发目标）
// - backend_timeout_ms proxy→backend 转发超时（覆盖 GdsPut/UcxPut/PutBlock）

#include <gflags/gflags.h>

DECLARE_int32(proxy_port);
DECLARE_string(bind_host);
DECLARE_int32(num_threads);
DECLARE_string(backend_endpoint);
DECLARE_int32(backend_timeout_ms);
