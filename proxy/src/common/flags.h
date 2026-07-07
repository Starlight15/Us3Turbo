#pragma once
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
