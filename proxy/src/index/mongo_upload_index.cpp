#include "proxy/src/index/mongo_upload_index.h"

#include <nlohmann/json.hpp>

#include "proxy/src/common/errors.h"
#include "proxy/src/common/flags.h"
#include "proxy/src/common/utils.h"
#include "proxy/src/index/mongo_schema.h"
#include "proxy/src/logging/logger.h"

namespace us3_turbo::proxy {

namespace mgo = ::us3_turbo::proxy::mongo;

std::string MongoUploadIndex::Create(const std::string& bucket, const std::string& key,
                                     PutDataPath path) {
  const std::string upload_id = utils::GenUuid();
  const std::string obj_id = utils::GenUuid();

  int ret = client_->InsertMinit(upload_id, static_cast<std::uint32_t>(FLAGS_bucket_id),
                                 key, obj_id, static_cast<int>(path));
  if (ret != 0) {
    LOG_SYS_ERROR("InsertMinit failed: upload_id={} ret={}", upload_id, ret);
    return {};
  }

  return upload_id;
}

bool MongoUploadIndex::Get(const std::string& upload_id, UploadRecord& out) {
  std::string doc_json;
  int ret = client_->QueryMinit(upload_id, doc_json);
  if (ret == -1) return false;  // not found
  if (ret != 0) return false;

  try {
    auto doc = nlohmann::json::parse(doc_json);
    out.upload_id = doc[mgo::f::kUploadId].get<std::string>();
    out.bucket = "";  // bucket_id only; name lookup deferred to Phase 5
    out.key = doc[mgo::f::kKey].get<std::string>();
    out.obj_id = doc[mgo::f::kFirstObject].get<std::string>();
    out.block_size = doc.value(mgo::f::kMinitBlockSize, 16ULL * 1024 * 1024);
    out.merged_size = doc.value(mgo::f::kMergedSize, 0ULL);
    out.last_merged_part = doc.value(mgo::f::kLastMergedPart, 0);
    out.status = doc.value(mgo::f::kStatus, 0);

    /* path field: DBGate stores as number, may be string or int */
    int path_int = 1;
    if (doc.contains(mgo::f::kPath)) {
      if (doc[mgo::f::kPath].is_string()) {
        path_int = std::stoi(doc[mgo::f::kPath].get<std::string>());
      } else if (doc[mgo::f::kPath].is_number()) {
        path_int = doc[mgo::f::kPath].get<int>();
      }
    }
    out.path = static_cast<PutDataPath>(path_int);

    return true;
  } catch (const std::exception& e) {
    LOG_SYS_ERROR("Failed to parse minit doc: {}", e.what());
    return false;
  }
}

bool MongoUploadIndex::AddPart(const std::string& upload_id, const PartRecord& part) {
  int ret = client_->InsertPart(upload_id, part.part_number, part.file_offset,
                                part.part_size, part.etag, part.block_crcs);
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
      part.part_number = doc[mgo::f::kSeq].get<std::uint32_t>();
      part.file_offset = doc[mgo::f::kOffset].get<std::uint64_t>();
      part.part_size = doc[mgo::f::kSize].get<std::uint64_t>();
      part.etag = doc.value(mgo::f::kEtag, "");
      part.valid = true;  // persisted part implies successful upload

      // parse CRC array
      if (doc.contains(mgo::f::kCrc) && doc[mgo::f::kCrc].is_array()) {
        for (const auto& c : doc[mgo::f::kCrc]) {
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
  /* 1) delete parts, 2) delete minit — idempotent */
  client_->DeleteParts(upload_id);
  client_->DeleteMinit(upload_id);
}

void MongoUploadIndex::RemoveExpired(std::int64_t /*ttl_ms*/) {
  /* TTL managed by MongoDB TTL index; Phase 5 may add explicit cleanup */
}

bool MongoUploadIndex::UpdateMergedSize(const std::string& upload_id,
                                        std::uint64_t merged_size) {
  int ret = client_->UpdateMinit(upload_id, mgo::f::kMergedSize, merged_size);
  return ret == 0;
}

bool MongoUploadIndex::UpdateLastMergedPart(const std::string& upload_id,
                                            std::int32_t part_number) {
  int ret = client_->UpdateMinit(upload_id, mgo::f::kLastMergedPart,
                                 static_cast<std::uint64_t>(part_number));
  return ret == 0;
}

bool MongoUploadIndex::InsertFileIdx(const std::string& /*bucket*/,
                                     const std::string& key,
                                     const std::string& first_object,
                                     std::uint64_t block_size, std::uint64_t filesize,
                                     const std::string& hash) {
  int ret = client_->UpsertFileIdx(static_cast<std::uint32_t>(FLAGS_bucket_id), key,
                                   first_object, block_size, filesize, hash);
  return ret == 0;
}

bool MongoUploadIndex::GetFileIdx(const std::string& /*bucket*/, const std::string& key,
                                  FileIdxRecord& out) {
  std::string doc_json;
  int ret =
      client_->QueryFileIdx(static_cast<std::uint32_t>(FLAGS_bucket_id), key, doc_json);
  if (ret != 0) return false;  // not found or query failed — treat as 404

  try {
    auto doc = nlohmann::json::parse(doc_json);
    out.first_object = doc[mgo::f::kFirstObject].get<std::string>();
    out.block_size = doc[mgo::f::kFileIdxBlockSize].get<std::uint64_t>();  // 无下划线
    out.filesize = doc[mgo::f::kFilesize].get<std::uint64_t>();
    out.hash = doc.value(mgo::f::kHash, "");
    return true;
  } catch (const std::exception& e) {
    LOG_SYS_ERROR("Failed to parse fileidx doc: {}", e.what());
    return false;
  }
}

}  // namespace us3_turbo::proxy
