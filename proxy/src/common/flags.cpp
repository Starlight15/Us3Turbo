// flags.cpp — proxy 控制面命令行参数（gflags）集中定义。
//
// 既往 DEFINE 散置于 main.cpp 顶部，现抽出独立翻译单元；main.cpp 仅保留
// ParseCommandLineFlags 调用。引用方 #include "proxy/src/common/flags.h" 后用 FLAGS_xxx。

#include "proxy/src/common/flags.h"

DEFINE_int32(proxy_port, 9100, "proxy control-plane brpc port");
DEFINE_string(bind_host, "192.168.1.198", "Bind host for the brpc listener");
DEFINE_int32(num_threads, 4, "brpc worker thread count");
DEFINE_string(backend_endpoint, "192.168.1.198:9200",
              "backend data plane endpoint (GdsPut/UcxPut/PutBlock)");
DEFINE_int32(backend_timeout_ms, 30000,
             "Timeout (ms) for proxy→backend forward (GdsPut/UcxPut/PutBlock)");
