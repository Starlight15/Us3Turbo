#include "proxy/src/index/mongo_upload_index.h"

#include <nlohmann/json.hpp>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/common/utils.h"
#include "proxy/src/logging/logger.h"

namespace us3_turbo::proxy {

MongoUploadIndex::MongoUploadIndex(DBGateClient* client) : client_(client) {}

std::string MongoUploadIndex::Create(
    const std::string& bucket, const std::string& key,
    PutDataPath path) {
  const std::string upload_id = utils::GenUuid();
  const std::string obj_id = utils::GenUuid();

  // path → 字符串
  std::string path_str = std::to_string(static_cast<int>(path));

  int ret = client_->InsertMinit(
      upload_id,
      static_cast<std::uint32_t>(FLAGS_bucket_id),
      key,
      obj_id,
      path_str);
  if (ret != 0) {
    LOG_SYS_ERROR("InsertMinit failed: upload_id={} ret={}", upload_id, ret);
    return {};
  }

  return upload_id;
}

bool MongoUploadIndex::Get(const std::string& upload_id, UploadRecord& out) {
  std::string doc_json;
  int ret = client_->QueryMinit(upload_id, doc_json);
  if (ret == -1) return false;  // 未找到
  if (ret != 0) return false;

  try {
    auto doc = nlohmann::json::parse(doc_json);
    out.upload_id = doc["uploadid"].get<std::string>();
    out.bucket = "";  // MongoDB 只存 bucket_id，bucket 名称需额外查询（Phase 5）
    out.key = doc["key"].get<std::string>();
    out.obj_id = doc["first_object"].get<std::string>();
    out.block_size = doc.value("block_size", 4194304ULL);
    out.merged_size = doc.value("merged_size", 0ULL);
    out.last_merged_part = doc.value("last_merged_part", 0);
    out.status = doc.value("status", 0);

    // path 字段（DBGate 存为数字）
    int path_int = 1;  // 默认 PATH_GDS
    if (doc.contains("path")) {
      if (doc["path"].is_string()) {
        path_int = std::stoi(doc["path"].get<std::string>());
      } else if (doc["path"].is_number()) {
        path_int = doc["path"].get<int>();
      }
    }
    out.path = static_cast<PutDataPath>(path_int);

    return true;
  } catch (const std::exception& e) {
    LOG_SYS_ERROR("Failed to parse minit doc: {}", e.what());
    return false;
  }
}

bool MongoUploadIndex::AddPart(const std::string& upload_id,
                                const PartRecord& part) {
  // 构造 CRC 数组 JSON 字符串 "[123,456,789,...]"
  std::string crc_array = "[";
  for (std::size_t i = 0; i < part.block_crcs.size(); ++i) {
    if (i > 0) crc_array += ",";
    crc_array += std::to_string(part.block_crcs[i]);
  }
  crc_array += "]";

  int ret = client_->InsertPart(
      upload_id,
      part.part_number,
      part.file_offset,
      part.part_size,
      part.etag,
      crc_array);
  return ret == 0;
}

bool MongoUploadIndex::ListParts(const std::string& upload_id,
                                  std::vector<PartRecord>& out) {
  std::string docs_json;
  int ret = client_->QueryParts(upload_id, docs_json);
  if (ret != 0) return false;

  try {
    auto docs = nlohmann::json::parse(docs_json);
    if (!docs.is_array()) return false;

    for (const auto& doc : docs) {
      PartRecord part;
      part.part_number = doc["seq"].get<std::uint32_t>();
      part.file_offset = doc["offset"].get<std::uint64_t>();
      part.part_size = doc["size"].get<std::uint64_t>();
      part.etag = doc.value("etag", "");
      part.valid = true;  // MongoDB 里存的 part 一定是已成功上传的

      // 解析 CRC 数组
      if (doc.contains("crc") && doc["crc"].is_array()) {
        for (const auto& c : doc["crc"]) {
          part.block_crcs.push_back(c.get<std::uint32_t>());
        }
      }

      out.push_back(part);
    }
    return true;
  } catch (const std::exception& e) {
    LOG_SYS_ERROR("Failed to parse part docs: {}", e.what());
    return false;
  }
}

void MongoUploadIndex::Remove(const std::string& upload_id) {
  // 先删 part，再删 minit（幂等）
  client_->DeleteParts(upload_id);
  client_->DeleteMinit(upload_id);
}

void MongoUploadIndex::RemoveExpired(std::int64_t /*ttl_ms*/) {
  // MongoDB 持久化索引：TTL 由 MongoDB TTL 索引管理，此处 no-op
  // Phase 5 可选：查询所有 status=0 且 created_at 过期的 minit 并清理
}

bool MongoUploadIndex::UpdateMergedSize(const std::string& upload_id,
                                         std::uint64_t merged_size) {
  int ret = client_->UpdateMinit(upload_id, "merged_size", merged_size);
  return ret == 0;
}

bool MongoUploadIndex::UpdateLastMergedPart(const std::string& upload_id,
                                             std::int32_t part_number) {
  int ret = client_->UpdateMinit(upload_id, "last_merged_part",
                                  static_cast<std::uint64_t>(part_number));
  return ret == 0;
}

bool MongoUploadIndex::InsertFileIdx(
    const std::string& /*bucket*/,
    const std::string& key,
    const std::string& first_object,
    std::uint64_t block_size,
    std::uint64_t filesize,
    const std::string& hash) {
  int ret = client_->UpsertFileIdx(
      static_cast<std::uint32_t>(FLAGS_bucket_id),
      key,
      first_object,
      block_size,
      filesize,
      hash);
  return ret == 0;
}

}  // namespace us3_turbo::proxy
