#pragma once

namespace us3_turbo::proxy::mongo {

/* (db, collection) 成对出现，杜绝 db/collection 配错。 */
struct Target {
  const char* db;
  const char* col;
};
inline constexpr Target kFileIdx{"fileidx_db", "fileidx_col"};
inline constexpr Target kMinit{"mupload_db", "minit_col"};
inline constexpr Target kPartList{"mupload_db", "partlist_col"};

/* 字段名单一事实源，读写两侧共用。 */
namespace f {
inline constexpr auto kBucketId = "bucket_id";
inline constexpr auto kKey = "key";
inline constexpr auto kFirstObject = "first_object";
inline constexpr auto kFilesize = "filesize";
inline constexpr auto kHash = "hash";
inline constexpr auto kFinished = "finished";
inline constexpr auto kDelete = "delete";
inline constexpr auto kUploadId = "uploadid";
inline constexpr auto kPath = "path";
inline constexpr auto kMergedSize = "merged_size";
inline constexpr auto kLastMergedPart = "last_merged_part";
inline constexpr auto kStatus = "status";
inline constexpr auto kSeq = "seq";
inline constexpr auto kOffset = "offset";
inline constexpr auto kSize = "size";
inline constexpr auto kEtag = "etag";
inline constexpr auto kCrc = "crc";
// 两个"块大小"字段刻意不同名，分别定义、不要合并：
inline constexpr auto kFileIdxBlockSize =
    "blocksize";  // fileidx 沿用 s3proxy schema（无下划线）
inline constexpr auto kMinitBlockSize =
    "block_size";  // minit 用 Us3Turbo 自有 schema（有下划线）
}  // namespace f

}  // namespace us3_turbo::proxy::mongo
