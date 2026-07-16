#include "proxy/src/index/dbgate_client.h"

#include <arpa/inet.h>
#include <cstring>
#include <random>

#include <nlohmann/json.hpp>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/index/mongo_schema.h"
#include "proxy/src/logging/logger.h"
#include "ucloud.pb.h"
#include "umgogate.pb.h"

namespace us3_turbo::proxy {

namespace mgo = ::us3_turbo::proxy::mongo;

bool DBGateClient::ParseEndpoint(const std::string& endpoint, std::string& host,
                                 int& port) {
  const auto pos = endpoint.rfind(':');
  if (pos == std::string::npos) return false;
  host = endpoint.substr(0, pos);
  try {
    port = std::stoi(endpoint.substr(pos + 1));
  } catch (...) {
    return false;
  }
  return !host.empty() && port > 0 && port <= 65535;
}

std::pair<std::size_t, TcpConnection*> DBGateClient::AcquireConn() {
  if (conns_.empty()) return {kInvalidConnIndex, nullptr};

  std::size_t start = next_idx_.fetch_add(1, std::memory_order_relaxed) % conns_.size();
  for (std::size_t i = 0; i < conns_.size(); ++i) {
    std::size_t idx = (start + i) % conns_.size();
    auto* conn = conns_[idx].get();
    if (conn->alive()) return {idx, conn};
    std::lock_guard<std::mutex> lk(*conn_mutexes_[idx]);
    if (!conn->alive() && conn->Connect()) return {idx, conn};
  }
  return {kInvalidConnIndex, nullptr};
}

int DBGateClient::SendAndRecv(const std::vector<char>& req_buf,
                              std::vector<char>& out_rsp_buf) {
  /* 连接级失败重试: 对端(dbgate)空闲关闭后, 池中连接第一笔请求必失败。
   * 失败后立即 Close 当前连接, 下次 AcquireConn 跳过 !alive 连接取下一条
   * (或触发 Connect 重连)。固定重试 1 次(共 2 次尝试), 吸收单次连接级故障。
   * 见 review/fix_proxy_connection_retry.md (P1)。 */
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto [idx, conn] = AcquireConn();
    if (!conn) {
      if (attempt == 0) {
        LOG_SYS_WARN("SendAndRecv to dbgate: AcquireConn failed, retry once");
        continue;  // 池中可能有其它连接或可重连
      }
      LOG_SYS_ERROR(
          "SendAndRecv to dbgate: all connections unavailable "
          "after 2 attempts");
      return PROXY_ERR_BACKEND_UNAVAILABLE;
    }

    std::lock_guard<std::mutex> lock(*conn_mutexes_[idx]);

    /* 发送: [4B大端长度][req_buf] */
    std::uint32_t req_len = static_cast<std::uint32_t>(req_buf.size());
    std::uint32_t req_len_be = htonl(req_len);

    bool ok = true;
    if (conn->SendAll(&req_len_be, 4) != 0) {
      LOG_SYS_ERROR("SendAll length failed");
      ok = false;
    } else if (conn->SendAll(req_buf.data(), req_buf.size()) != 0) {
      LOG_SYS_ERROR("SendAll body failed");
      ok = false;
    }

    /* 接收: [4B大端长度][rsp_buf] */
    std::uint32_t rsp_len_be = 0;
    if (ok && conn->RecvAll(&rsp_len_be, 4) != 0) {
      LOG_SYS_ERROR("RecvAll length failed");
      ok = false;
    }
    std::uint32_t rsp_len = ok ? ntohl(rsp_len_be) : 0;

    if (ok && rsp_len > 16 * 1024 * 1024) {
      LOG_SYS_ERROR("Response length too large: {}", rsp_len);
      conn->set_dead();
      conn->Close();
      return PROXY_ERR_BACKEND_PROTOCOL;  // 协议错误不重试
    }

    if (ok) {
      out_rsp_buf.resize(rsp_len);
      if (conn->RecvAll(out_rsp_buf.data(), rsp_len) != 0) {
        LOG_SYS_ERROR("RecvAll body failed");
        ok = false;
      }
    }

    if (ok) {
      return 0;  // 成功
    }

    /* 失败: 立即销毁当前连接, 准备重试 */
    conn->set_dead();
    conn->Close();

    /* 对端空闲关闭时, 池中所有连接可能同时失效(同批创建 → 同时空闲 → 同时
     * 被对端关)。仅 set_dead 当前连接不够: AcquireConn 仍会返回其它
     * alive_=true 但实际已死的僵尸连接。主动标记所有连接为 dead, 让下次
     * AcquireConn 走 Connect() 建新连接。set_dead() 原子操作线程安全;
     * Close() 由后续 Connect() 在检测到 fd_>=0 时完成。 */
    if (attempt == 0) {
      for (auto& c : conns_) {
        c->set_dead();
      }
      LOG_SYS_WARN(
          "SendAndRecv to dbgate failed (SendAll/RecvAll error), "
          "invalidated all pool conns, retry with fresh conn");
      continue;
    }
    LOG_SYS_ERROR("SendAndRecv to dbgate failed after 2 attempts");
  }
  return PROXY_ERR_BACKEND_IO;
}

int DBGateClient::ExecuteMgo(const std::string& mgo_req_serialized,
                             std::string& out_mgo_rsp_serialized) {
  /* 反序列化请求 */
  ucloud::umgogate::ExecuteMgoRequest mgo_req;
  if (!mgo_req.ParseFromString(mgo_req_serialized)) {
    LOG_SYS_ERROR("Failed to parse ExecuteMgoRequest");
    return PROXY_ERR_INTERNAL;
  }

  /* 构造 UMessage */
  ucloud::UMessage msg;

  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<std::uint32_t> dist;

  auto* head = msg.mutable_head();
  head->set_version(1);
  head->set_magic_flag(0x12340987);
  head->set_random_num(dist(gen));
  head->set_flow_no(static_cast<::google::protobuf::uint32>(
      flow_no_.fetch_add(1, std::memory_order_relaxed)));
  head->set_session_no("0");
  head->set_message_type(150000);  // EXECUTE_MGO_REQUEST
  head->set_worker_index(0);
  head->set_source_entity(dist(gen));

  /* 设置 body extension */
  msg.mutable_body()
      ->MutableExtension(ucloud::umgogate::execute_mgo_request)
      ->CopyFrom(mgo_req);

  std::string serialized;
  if (!msg.SerializeToString(&serialized)) {
    LOG_SYS_ERROR("Failed to serialize UMessage");
    return PROXY_ERR_INTERNAL;
  }

  std::vector<char> req_buf(serialized.begin(), serialized.end());

  /* 收发 */
  std::vector<char> rsp_buf;
  int ret = SendAndRecv(req_buf, rsp_buf);
  if (ret != 0) return ret;

  /* 解析响应 */
  ucloud::UMessage rsp_msg;
  if (!rsp_msg.ParseFromArray(rsp_buf.data(), static_cast<int>(rsp_buf.size()))) {
    LOG_SYS_ERROR("Failed to parse UMessage response");
    return PROXY_ERR_BACKEND_PROTOCOL;
  }

  if (!rsp_msg.has_head() || rsp_msg.head().message_type() != 150001) {
    LOG_SYS_ERROR("Unexpected message_type in response: {}",
                  rsp_msg.has_head() ? rsp_msg.head().message_type() : 0);
    return PROXY_ERR_BACKEND_PROTOCOL;
  }

  if (!rsp_msg.has_body() ||
      !rsp_msg.body().HasExtension(ucloud::umgogate::execute_mgo_response)) {
    LOG_SYS_ERROR("Missing execute_mgo_response extension in response body");
    return PROXY_ERR_BACKEND_PROTOCOL;
  }

  const auto& mgo_rsp =
      rsp_msg.body().GetExtension(ucloud::umgogate::execute_mgo_response);

  /* 检查 ret_code */
  if (!mgo_rsp.has_rc() || mgo_rsp.rc().retcode() != 0) {
    int ret_code = mgo_rsp.has_rc() ? mgo_rsp.rc().retcode() : -1;
    std::string ret_msg = mgo_rsp.has_rc() ? mgo_rsp.rc().error_message() : "";
    LOG_SYS_ERROR("DBGate returned error: ret_code={} msg={}", ret_code, ret_msg);
    return PROXY_ERR_BACKEND_FAILED;
  }

  if (!mgo_rsp.SerializeToString(&out_mgo_rsp_serialized)) {
    LOG_SYS_ERROR("Failed to serialize ExecuteMgoResponse");
    return PROXY_ERR_INTERNAL;
  }

  return 0;
}

/* 内部辅助: 构造请求 -> 序列化 -> ExecuteMgo -> 反序列化响应 */
namespace {

/* 设置 db + collection */
void SetTarget(ucloud::umgogate::ExecuteMgoRequest& r, const mgo::Target& t) {
  r.set_db(t.db);
  r.set_collection(t.col);
}

/* 按 upload_id 构建 selector（JSON 字符串自动转义） */
std::string SelByUpload(const std::string& upload_id) {
  return nlohmann::json{{mgo::f::kUploadId, upload_id}}.dump();
}

/* 按 bucket_id + key 构建 selector */
std::string SelByBucketKey(std::uint32_t bucket_id, const std::string& key) {
  return nlohmann::json{{mgo::f::kBucketId, bucket_id}, {mgo::f::kKey, key}}.dump();
}

int ExecuteMgoHelper(DBGateClient& client, ucloud::umgogate::ExecuteMgoRequest& mgo_req,
                     ucloud::umgogate::ExecuteMgoResponse& out_rsp) {
  std::string req_serialized;
  if (!mgo_req.SerializeToString(&req_serialized)) {
    LOG_SYS_ERROR("Failed to serialize ExecuteMgoRequest");
    return PROXY_ERR_INTERNAL;
  }

  std::string rsp_serialized;
  int ret = client.ExecuteMgo(req_serialized, rsp_serialized);
  if (ret != 0) return ret;

  if (!out_rsp.ParseFromString(rsp_serialized)) {
    LOG_SYS_ERROR("Failed to parse ExecuteMgoResponse");
    return PROXY_ERR_BACKEND_PROTOCOL;
  }

  return 0;
}
}  // namespace

// ============================ fileidx_col ============================

int DBGateClient::UpsertFileIdx(std::uint32_t bucket_id, const std::string& key,
                                const std::string& first_object, std::uint64_t block_size,
                                std::uint64_t filesize, const std::string& hash) {
  ucloud::umgogate::ExecuteMgoRequest mgo_req;
  SetTarget(mgo_req, mgo::kFileIdx);
  mgo_req.set_optype(ucloud::umgogate::OP_UPDATE);

  const std::string selector = SelByBucketKey(bucket_id, key);

  /* s3proxy schema: blocksize 无下划线; 用 hash 存对象完整性 */
  nlohmann::json doc = {
      {mgo::f::kBucketId, bucket_id},
      {mgo::f::kKey, key},
      {mgo::f::kFirstObject, first_object},
      {mgo::f::kFileIdxBlockSize, block_size},
      {mgo::f::kFilesize, filesize},
      {mgo::f::kHash, hash},
      {mgo::f::kFinished, 1},
      {mgo::f::kDelete, false},
  };

  auto* update_pair = mgo_req.mutable_op_update_req()->add_pairs();
  update_pair->set_selector(selector);
  update_pair->set_doc(doc.dump());
  update_pair->set_upsert(true);
  update_pair->set_oldly(true);

  ucloud::umgogate::ExecuteMgoResponse rsp;
  return ExecuteMgoHelper(*this, mgo_req, rsp);
}

int DBGateClient::QueryFileIdx(std::uint32_t bucket_id, const std::string& key,
                               std::string& out_doc) {
  ucloud::umgogate::ExecuteMgoRequest mgo_req;
  SetTarget(mgo_req, mgo::kFileIdx);
  mgo_req.set_optype(ucloud::umgogate::OP_FIND);

  const std::string selector = SelByBucketKey(bucket_id, key);
  mgo_req.mutable_op_find_req()->set_selector(selector);

  ucloud::umgogate::ExecuteMgoResponse rsp;
  int ret = ExecuteMgoHelper(*this, mgo_req, rsp);
  if (ret != 0) return ret;

  if (!rsp.has_op_find_rsp() || rsp.op_find_rsp().results_size() == 0) {
    return -1;  // 未找到
  }

  out_doc = rsp.op_find_rsp().results(0);
  return 0;
}

// ============================ minit_col ============================

int DBGateClient::InsertMinit(const std::string& upload_id, std::uint32_t bucket_id,
                              const std::string& key, const std::string& first_object,
                              int path) {
  ucloud::umgogate::ExecuteMgoRequest mgo_req;
  SetTarget(mgo_req, mgo::kMinit);
  mgo_req.set_optype(ucloud::umgogate::OP_INSERT);

  nlohmann::json doc = {
      {mgo::f::kUploadId, upload_id},
      {mgo::f::kBucketId, bucket_id},
      {mgo::f::kKey, key},
      {mgo::f::kFirstObject, first_object},
      {mgo::f::kPath, path},
      {mgo::f::kMinitBlockSize, static_cast<std::uint64_t>(FLAGS_multipart_part_size)},
      {mgo::f::kMergedSize, 0},
      {mgo::f::kLastMergedPart, 0},
      {mgo::f::kStatus, 0},
  };

  mgo_req.mutable_op_insert_req()->add_doc(doc.dump());

  ucloud::umgogate::ExecuteMgoResponse rsp;
  return ExecuteMgoHelper(*this, mgo_req, rsp);
}

int DBGateClient::QueryMinit(const std::string& upload_id, std::string& out_doc) {
  ucloud::umgogate::ExecuteMgoRequest mgo_req;
  SetTarget(mgo_req, mgo::kMinit);
  mgo_req.set_optype(ucloud::umgogate::OP_FIND);

  const std::string selector = SelByUpload(upload_id);
  mgo_req.mutable_op_find_req()->set_selector(selector);

  ucloud::umgogate::ExecuteMgoResponse rsp;
  int ret = ExecuteMgoHelper(*this, mgo_req, rsp);
  if (ret != 0) return ret;

  /* 检查是否有结果 */
  if (!rsp.has_op_find_rsp() || rsp.op_find_rsp().results_size() == 0) {
    return -1;  // 未找到
  }

  out_doc = rsp.op_find_rsp().results(0);
  return 0;
}

int DBGateClient::UpdateMinit(const std::string& upload_id, const std::string& field_name,
                              std::uint64_t value) {
  ucloud::umgogate::ExecuteMgoRequest mgo_req;
  SetTarget(mgo_req, mgo::kMinit);
  mgo_req.set_optype(ucloud::umgogate::OP_UPDATE);

  const std::string selector = SelByUpload(upload_id);

  nlohmann::json doc = {{"$set", {{field_name, value}}}};

  auto* update_pair = mgo_req.mutable_op_update_req()->add_pairs();
  update_pair->set_selector(selector);
  update_pair->set_doc(doc.dump());

  ucloud::umgogate::ExecuteMgoResponse rsp;
  return ExecuteMgoHelper(*this, mgo_req, rsp);
}

int DBGateClient::DeleteMinit(const std::string& upload_id) {
  ucloud::umgogate::ExecuteMgoRequest mgo_req;
  SetTarget(mgo_req, mgo::kMinit);
  mgo_req.set_optype(ucloud::umgogate::OP_DELETE);

  const std::string selector = SelByUpload(upload_id);
  mgo_req.mutable_op_delete_req()->set_selector(selector);

  ucloud::umgogate::ExecuteMgoResponse rsp;
  int ret = ExecuteMgoHelper(*this, mgo_req, rsp);
  // 幂等: 未找到也返回 0
  return ret;
}

// ============================ partlist_col ============================

int DBGateClient::InsertPart(const std::string& upload_id, std::uint32_t part_number,
                             std::uint64_t offset, std::uint64_t size,
                             const std::string& etag,
                             const std::vector<std::uint32_t>& block_crcs) {
  ucloud::umgogate::ExecuteMgoRequest mgo_req;
  SetTarget(mgo_req, mgo::kPartList);
  mgo_req.set_optype(ucloud::umgogate::OP_INSERT);

  nlohmann::json doc = {
      {mgo::f::kUploadId, upload_id},
      {mgo::f::kSeq, part_number},
      {mgo::f::kOffset, offset},
      {mgo::f::kSize, size},
      {mgo::f::kEtag, etag},
      {mgo::f::kCrc, block_crcs},  // vector → JSON array 自动
  };

  mgo_req.mutable_op_insert_req()->add_doc(doc.dump());

  ucloud::umgogate::ExecuteMgoResponse rsp;
  return ExecuteMgoHelper(*this, mgo_req, rsp);
}

int DBGateClient::QueryParts(const std::string& upload_id, std::string& out_docs) {
  ucloud::umgogate::ExecuteMgoRequest mgo_req;
  SetTarget(mgo_req, mgo::kPartList);
  mgo_req.set_optype(ucloud::umgogate::OP_FIND);

  const std::string selector = SelByUpload(upload_id);
  mgo_req.mutable_op_find_req()->set_selector(selector);

  ucloud::umgogate::ExecuteMgoResponse rsp;
  int ret = ExecuteMgoHelper(*this, mgo_req, rsp);
  if (ret != 0) return ret;

  /* 返回 JSON 数组 */
  out_docs = "[";
  if (rsp.has_op_find_rsp()) {
    for (int i = 0; i < rsp.op_find_rsp().results_size(); ++i) {
      if (i > 0) out_docs += ",";
      out_docs += rsp.op_find_rsp().results(i);
    }
  }
  out_docs += "]";
  return 0;
}

int DBGateClient::DeleteParts(const std::string& upload_id) {
  ucloud::umgogate::ExecuteMgoRequest mgo_req;
  SetTarget(mgo_req, mgo::kPartList);
  mgo_req.set_optype(ucloud::umgogate::OP_DELETE);

  const std::string selector = SelByUpload(upload_id);
  mgo_req.mutable_op_delete_req()->set_selector(selector);

  ucloud::umgogate::ExecuteMgoResponse rsp;
  int ret = ExecuteMgoHelper(*this, mgo_req, rsp);
  return ret;  // 幂等: 未找到也返回 0
}

}  // namespace us3_turbo::proxy
