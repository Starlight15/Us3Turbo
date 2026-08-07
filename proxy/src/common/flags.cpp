#include "proxy/src/common/flags.h"

DEFINE_int32(proxy_port, 9100, "proxy control-plane brpc port");
DEFINE_string(bind_host, "0.0.0.0", "Bind host for the brpc listener (0.0.0.0 = all interfaces)");
DEFINE_int32(num_threads, 16, "brpc worker thread count "
             "(perf-tuned 默认 16，须 ≈ backend_conn_pool_size，见 "
             "docs/NUM_THREADS_DEEP_ANALYSIS.md)");
DEFINE_string(backend_endpoint, "192.168.1.198:24000",
              "backend ufile-ac TCP endpoint for single-step GdsPut/RdmaPut "
              "(doc F5; setid must match backend [common] setid). "
              "本测试环境 fallback；跨环境部署须显式设置。");
DEFINE_int32(backend_timeout_ms, 30000,
             "Timeout (ms) for proxy→backend forward (GdsPut/RdmaPut/PutBlock)");
DEFINE_int32(backend_setid, 1,
             "ufile-ac setid, must match backend [common] setid "
             "(doc F3; mismatch → backend ForceClose)");
DEFINE_int64(max_single_put_bytes, 4LL * 1024 * 1024,
             "max object size (bytes) for single-step GdsPut/RdmaPut; larger "
             "objects must use multipart");
DEFINE_int64(multipart_part_size, 8LL * 1024 * 1024,
             "part size (bytes) for multipart upload, also the on-disk block "
             "size (each part is written as one block) "
             "(perf-tuned 默认 8M，见 docs/PART_SIZE_DEEP_ANALYSIS.md)");
DEFINE_string(log_level, "info", "app log level: debug/info/warn/error");
DEFINE_int32(log_max_size_mb, 50, "max size per app log file (MB); rotates when exceeded");
DEFINE_int32(log_max_files, 10, "max number of rotated app log files to keep");
DEFINE_int32(backend_conn_pool_size, 16,
             "backend connection pool size (recommend: ≈ num_threads for best "
             "throughput; perf-tuned 默认 16，见 docs/CONN_POOL_DEEP_ANALYSIS.md)");
DEFINE_int32(backend_send_recv_max_retry, 2,
             "max retry attempts (inclusive) for SendAndRecv on connection-level "
             "failure; protocol errors are not retried");
DEFINE_string(dbgate_endpoint, "192.168.1.198:20165",
              "DBGate proxy endpoint for MongoDB operations. "
              "本测试环境 fallback；跨环境部署须显式设置。");
DEFINE_int32(dbgate_timeout_ms, 5000, "DBGate request timeout in milliseconds");
DEFINE_int32(dbgate_conn_pool_size, 4,
             "DBGate connection pool size (recommend: match num_threads)");
DEFINE_int32(dbgate_send_recv_max_retry, 2,
             "max retry attempts (inclusive) for DBGate SendAndRecv on "
             "connection-level failure; protocol errors are not retried");
DEFINE_int32(bucket_id, 1,
             "Bucket ID for index operations (temporary hardcoded, Phase 5 "
             "will query bucketidx_col)");
DEFINE_bool(enable_perf_stats, false,
            "emit [perf/proxy] per-RPC stage timing (us) for bottleneck "
            "localization; off by default, zero overhead when disabled");
DEFINE_int32(snowflake_node_id, 1,
            "snowflake trace_id 的 node 位(0~1023);多 proxy 实例须不同以保证 "
            "trace_id 全局唯一(对齐 s3proxy 的 snowflake node)");
