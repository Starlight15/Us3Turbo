#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "control_plane.pb.h"

namespace us3_turbo::proxy {

// block 级元数据（一个 part 按 kBlockSize 拆成多个 block，每块单独写 ufile-ac）。
struct BlockInfo {
  std::string   key;            // ufile-ac 中的 block key
                               // 格式：mp/{uuid32}/p{part_no:04u}b{block_no:02u}（≤48）
  std::uint64_t offset{0};      // block 在 part 数据中的偏移（= gpu/source offset）
  std::uint64_t size{0};        // block 大小（通常 4MB，末块可能更小）
  std::uint32_t crc32c{0};      // block 数据 CRC32C（ufile-ac 返回）
};

/*
 * part 元数据（对齐 s3proxy S3PartInfo）
 * 当前阶段：内存实现，字段语义对齐 s3proxy，为后续 MongoDB 持久化做准备
 */
struct PartRecord {
  std::uint32_t part_number{0};   // 1-based
  std::uint64_t part_size{0};
  std::string   etag;             // part 级 ETag（client 校验用）
  std::int64_t  upload_time_ms{0};

  // ✅ 新增字段（对齐 s3proxy）
  std::uint64_t file_offset{0};   // 该 part 在最终文件的偏移
  bool          valid{false};     // 是否完整上传
  std::uint64_t unmerge_size{0};  // 未合并大小（Us3Turbo 恒为 0）

  /*
   * 该 part 的所有 block crcs（用于对象内容哈希 = s3proxy US3Etags）。
   * 16MB 对齐后每 part 固定 4 blocks（除末 part），写入时按块号顺序存储。
   * CompleteUpload 时从已排序 parts 顺序拼接得全局有序 crcs。
   */
  std::vector<std::uint32_t> block_crcs;
};

/*
 * upload 级元数据（对齐 s3proxy S3MinitIdxInfo）
 * 当前阶段：内存实现，字段语义对齐，为后续 MongoDB 持久化做准备
 */
struct UploadRecord {
  std::string  upload_id;
  std::string  bucket;
  std::string  key;
  PutDataPath  path{PATH_NONE};
  std::int64_t created_at_ms{0};

  // ✅ 新增字段（对齐 s3proxy）
  std::string   obj_id;              // 最终对象 ID（= s3proxy ObjId）
  std::uint64_t block_size{4194304}; // 4MB 固定
  std::uint64_t merged_size{0};      // 已合并字节数（每 UploadPart 累加，Complete 用作总大小）
  std::int32_t  last_merged_part{0}; // 最后合并到的 part_number（对齐 s3proxy，供未来续传/持久化）
  std::int32_t  status{0};           // 0=进行中, 1=完成, 2=中止
  // 注：per-block crcs 存 PartRecord.block_crcs（有序），不在此处平铺累积。
};

// 纯被动元数据存储接口。内存 mock 与后续 MongoDB 实现同一接口，可无差别替换。
// 不含任何校验 / etag 计算 / client 比对（全在服务层）。
class IUploadIndex {
 public:
  virtual ~IUploadIndex() = default;

  /** @brief 创建新会话，返回 upload_id（UUID）。 */
  [[nodiscard]] virtual std::string Create(
      const std::string& bucket, const std::string& key,
      PutDataPath path) = 0;

  /** @brief 读会话（不存在返回 false）。纯读，不含业务判断。 */
  [[nodiscard]] virtual bool Get(
      const std::string& upload_id, UploadRecord& out) = 0;

  /** @brief 追加/覆盖 part（同 part_number 覆盖）。不存在返回 false。 */
  [[nodiscard]] virtual bool AddPart(
      const std::string& upload_id, const PartRecord& part) = 0;

  /** @brief 列出某会话所有 part（未排序，排序/校验由服务层做）。 */
  [[nodiscard]] virtual bool ListParts(
      const std::string& upload_id, std::vector<PartRecord>& out) = 0;

  /** @brief 删除会话（幂等）。 */
  virtual void Remove(const std::string& upload_id) = 0;

  /** @brief 删除超过 ttl_ms 的会话（后台清理线程调用）。 */
  virtual void RemoveExpired(std::int64_t ttl_ms) = 0;

  // ✅ 新增接口（增量写索引）

  /*
   * 更新已合并大小（每 part 完成后调用）
   * 对齐 s3proxy merged_size 字段，虽 Us3Turbo 无流式合并但保持兼容
   */
  [[nodiscard]] virtual bool UpdateMergedSize(
      const std::string& upload_id,
      std::uint64_t merged_size) = 0;

  /*
   * 更新最后合并 part 号（每 part 完成后调用）
   * 对齐 s3proxy last_merged_part_num 字段
   */
  [[nodiscard]] virtual bool UpdateLastMergedPart(
      const std::string& upload_id,
      std::int32_t part_number) = 0;

  /**
   * @brief 写 fileidx_col（对象元数据）
   *
   * single_put 和 multipart Complete 调用，写入对象索引供 s3proxy 读取
   *
   * @return true=成功，false=失败
   */
  [[nodiscard]] virtual bool InsertFileIdx(
      const std::string& bucket,
      const std::string& key,
      const std::string& first_object,
      std::uint64_t block_size,
      std::uint64_t filesize,
      const std::string& hash) = 0;
};

}  // namespace us3_turbo::proxy
