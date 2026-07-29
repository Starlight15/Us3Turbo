#pragma once
#include <gflags/gflags.h>

DECLARE_int32(proxy_port);
DECLARE_string(bind_host);
DECLARE_int32(num_threads);
DECLARE_string(backend_endpoint);
DECLARE_int32(backend_timeout_ms);
DECLARE_int32(backend_setid);
DECLARE_int64(upload_ttl_ms);
DECLARE_int64(upload_ttl_scan_interval_ms);
DECLARE_int64(max_single_put_bytes);
DECLARE_int64(multipart_part_size);
DECLARE_string(log_level);
DECLARE_int32(log_max_size_mb);
DECLARE_int32(log_max_files);
DECLARE_int32(backend_conn_pool_size);
DECLARE_int32(backend_send_recv_max_retry);
DECLARE_string(dbgate_endpoint);
DECLARE_int32(dbgate_timeout_ms);
DECLARE_int32(dbgate_conn_pool_size);
DECLARE_int32(dbgate_send_recv_max_retry);
DECLARE_int32(bucket_id);
DECLARE_string(mongo_db_name);
