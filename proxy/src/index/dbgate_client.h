#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "proxy/src/storage/tcp_connection.h"
#include "us3_turbo/common/logger.h"

namespace us3_turbo::proxy {

/* DBGate 客户端：裸 TCP 调用 MongoDB；协议 [4B大端长度][protobuf UMessage]
 * 连接池：轮询 + 惰性重连（复用 TcpConnection，策略同 UfileAcClient）
 * 线程安全：每连接独立 mutex，序列化请求-响应对 */
class DBGateClient {
 public:
  // AcquireConn 无可用连接时返回的哨兵索引（区别于合法下标）
  static constexpr std::size_t kInvalidConnIndex = static_cast<std::size_t>(-1);

  DBGateClient(const std::string& endpoint, int timeout_ms, int pool_size)
      : timeout_ms_(timeout_ms) {
    if (!ParseEndpoint(endpoint, host_, port_)) {
      LOG_SYS_WARN(
          "dbgate_endpoint '{}' parse failed (expect host:port), "
          "DBGate operations disabled",
          endpoint);
      return;
    }

    conns_.reserve(static_cast<std::size_t>(pool_size));
    conn_mutexes_.reserve(static_cast<std::size_t>(pool_size));
    std::size_t connected = 0;
    for (int i = 0; i < pool_size; ++i) {
      auto conn = std::make_unique<TcpConnection>(host_, port_, timeout_ms_);
      if (conn->Connect()) ++connected;
      conns_.push_back(std::move(conn));
      conn_mutexes_.push_back(std::make_unique<std::mutex>());
    }

    LOG_SYS_INFO(
        "DBGateClient initialized: endpoint={} pool_size={} "
        "connected={}",
        endpoint, pool_size, connected);
  }

  // ========== fileidx_col ==========

  /* Upsert fileidx_col 文档（single_put / multipart Complete） */
  [[nodiscard]] int UpsertFileIdx(std::uint32_t bucket_id, const std::string& key,
                                  const std::string& first_object, std::uint64_t block_size,
                                  std::uint64_t filesize, const std::string& hash);

  /* Query fileidx_col 文档（GetObject 第一步）; Returns 0=ok, -1=not found,
   * other=error */
  [[nodiscard]] int QueryFileIdx(std::uint32_t bucket_id, const std::string& key,
                                 std::string& out_doc);

  // ========== minit_col ==========

  /* Insert minit_col 文档（CreateUpload） */
  [[nodiscard]] int InsertMinit(const std::string& upload_id, std::uint32_t bucket_id,
                                const std::string& key, const std::string& first_object, int path);

  /* Query minit_col 文档（GetUpload）; Returns 0=ok, -1=not found, other=error
   */
  [[nodiscard]] int QueryMinit(const std::string& upload_id, std::string& out_doc);

  /* Update minit_col 字段（merged_size / last_merged_part） */
  [[nodiscard]] int UpdateMinit(const std::string& upload_id, const std::string& field_name,
                                std::uint64_t value);

  /* Delete minit_col 文档（AbortUpload/CompleteUpload） */
  [[nodiscard]] int DeleteMinit(const std::string& upload_id);

  // ========== part_col ==========

  /* Insert part_col 文档（UploadPart）; block_crcs 序列化为 JSON 数组 */
  [[nodiscard]] int InsertPart(const std::string& upload_id, std::uint32_t part_number,
                               std::uint64_t offset, std::uint64_t size, const std::string& etag,
                               const std::vector<std::uint32_t>& block_crcs);

  /* Query part_col 文档列表（CompleteUpload） */
  [[nodiscard]] int QueryParts(const std::string& upload_id, std::string& out_docs);

  /* Delete part_col 文档（AbortUpload/CompleteUpload） */
  [[nodiscard]] int DeleteParts(const std::string& upload_id);

  /* 通用 MongoDB 操作骨架：构造 UMessage + 发送 + 解析 ExecuteMgoResponse
   * 入参/出参为序列化 protobuf 字符串，避免头文件依赖 proto 类型
   * Returns 0=成功，非0=错误码 */
  [[nodiscard]] int ExecuteMgo(const std::string& mgo_req_serialized,
                               std::string& out_mgo_rsp_serialized);

 private:
  // 拆分 "host:port" → {host, port}
  static bool ParseEndpoint(const std::string& endpoint, std::string& host, int& port);

  // 轮询取连接（跳过坏连接，惰性重连）
  std::pair<std::size_t, TcpConnection*> AcquireConn();

  // 通用收发骨架：[4字节长度][UMessage] 协议
  int SendAndRecv(const std::vector<char>& req_buf, std::vector<char>& out_rsp_buf);

  int timeout_ms_;
  std::string host_;
  int port_{0};
  std::vector<std::unique_ptr<TcpConnection>> conns_;
  std::vector<std::unique_ptr<std::mutex>> conn_mutexes_;
  std::atomic<std::uint64_t> next_idx_{0};
  std::atomic<std::uint64_t> flow_no_{0};
};

}  // namespace us3_turbo::proxy
