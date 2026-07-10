#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "proxy/src/storage/tcp_connection.h"

namespace us3_turbo::proxy {

/**
 * @brief DBGate 客户端：通过裸 TCP 协议调用 MongoDB 操作
 *
 * 协议：[4字节大端长度][protobuf UMessage]
 * - 长度 = UMessage 序列化后的字节数（不含长度字段本身）
 * - UMessage = ucloud.proto 定义的消息结构
 *
 * 连接池：轮询 + 惰性重连（复用 TcpConnection，策略同 UfileAcClient）
 *
 * 线程安全：每连接独立 mutex，序列化请求-响应对
 */
class DBGateClient {
 public:
  // AcquireConn 无可用连接时返回的哨兵索引（区别于合法下标）
  static constexpr std::size_t kInvalidConnIndex = static_cast<std::size_t>(-1);

  DBGateClient(const std::string& endpoint, int timeout_ms, int pool_size);

  // ========== fileidx_col ==========

  /** @brief Upsert fileidx_col 文档（single_put / multipart Complete） */
  [[nodiscard]] int UpsertFileIdx(
      std::uint32_t bucket_id,
      const std::string& key,
      const std::string& first_object,
      std::uint64_t block_size,
      std::uint64_t filesize,
      const std::string& hash);

  // ========== minit_col ==========

  /** @brief Insert minit_col 文档（CreateUpload） */
  [[nodiscard]] int InsertMinit(
      const std::string& upload_id,
      std::uint32_t bucket_id,
      const std::string& key,
      const std::string& first_object,
      int path);

  /** @brief Query minit_col 文档（GetUpload）. Returns 0=ok, -1=not found, other=error */
  [[nodiscard]] int QueryMinit(
      const std::string& upload_id,
      std::string& out_doc);

  /** @brief Update minit_col 字段（merged_size / last_merged_part） */
  [[nodiscard]] int UpdateMinit(
      const std::string& upload_id,
      const std::string& field_name,
      std::uint64_t value);

  /** @brief Delete minit_col 文档（AbortUpload/CompleteUpload） */
  [[nodiscard]] int DeleteMinit(const std::string& upload_id);

  // ========== part_col ==========

  /** @brief Insert part_col 文档（UploadPart） */
  [[nodiscard]] int InsertPart(
      const std::string& upload_id,
      std::uint32_t part_number,
      std::uint64_t offset,
      std::uint64_t size,
      const std::string& etag,
      const std::string& crc_array);

  /** @brief Query part_col 文档列表（CompleteUpload） */
  [[nodiscard]] int QueryParts(
      const std::string& upload_id,
      std::string& out_docs);

  /** @brief Delete part_col 文档（AbortUpload/CompleteUpload） */
  [[nodiscard]] int DeleteParts(const std::string& upload_id);

  /**
   * @brief 通用 MongoDB 操作骨架
   *
   * 构造 UMessage + 发送 + 解析 ExecuteMgoResponse。
   * 参数为序列化的 protobuf 字符串，避免头文件依赖 proto 类型。
   *
   * @param mgo_req_serialized  已序列化的 ExecuteMgoRequest
   * @param out_mgo_rsp_serialized 输出已序列化的 ExecuteMgoResponse
   * @return 0=成功，非0=错误码
   */
  [[nodiscard]] int ExecuteMgo(const std::string& mgo_req_serialized,
                               std::string& out_mgo_rsp_serialized);

 private:
  // 拆分 "host:port" → {host, port}
  static bool ParseEndpoint(const std::string& endpoint,
                            std::string& host, int& port);

  // 轮询取连接（跳过坏连接，惰性重连）
  std::pair<std::size_t, TcpConnection*> AcquireConn();

  // 通用收发骨架：[4字节长度][UMessage] 协议
  int SendAndRecv(const std::vector<char>& req_buf,
                  std::vector<char>& out_rsp_buf);

  int      timeout_ms_;
  std::string host_;
  int         port_{0};
  std::vector<std::unique_ptr<TcpConnection>> conns_;
  std::vector<std::unique_ptr<std::mutex>>    conn_mutexes_;
  std::atomic<std::uint64_t> next_idx_{0};
  std::atomic<std::uint64_t> flow_no_{0};
};

}  // namespace us3_turbo::proxy
