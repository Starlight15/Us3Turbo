#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "control_plane.pb.h"

namespace us3_turbo::proxy {

/* part 按 kBlockSize 拆为多个 block，每块单独写 ufile-ac 的元数据 */
struct BlockInfo {
  std::string   key;            // 格式 mp/{uuid32}/p{part_no:04u}b{block_no:02u}（≤48）
  std::uint64_t offset{0};      // = gpu/source offset
  std::uint64_t size{0};        // 末块可能不足 4MB
  std::uint32_t crc32c{0};      // ufile-ac 返回
};

/* GET 用对象布局，对齐 fileidx_col schema */
struct FileIdxRecord {
  std::string   first_object;   // block key 前缀
  std::uint64_t block_size{0};
  std::uint64_t filesize{0};
  std::string   hash;
};

/* part 元数据，对齐 s3proxy S3PartInfo；当前内存实现，为后续 MongoDB 持久化做准备 */
struct PartRecord {
  std::uint32_t part_number{0};   // 1-based
  std::uint64_t part_size{0};
  std::string   etag;             // client 校验用
  std::int64_t  upload_time_ms{0};

  // 对齐 s3proxy 新增字段
  std::uint64_t file_offset{0};   // 最终文件内偏移
  bool          valid{false};     // 是否完整上传
  std::uint64_t unmerge_size{0};  // Us3Turbo 恒为 0

  /* per-block CRC32C 序列，4 blocks/part（除末 part），Complete 时拼接为全局有序 CRCs */
  std::vector<std::uint32_t> block_crcs;
};

/* upload 级元数据，对齐 s3proxy S3MinitIdxInfo；当前内存实现，为后续 MongoDB 持久化做准备 */
struct UploadRecord {
  std::string  upload_id;
  std::string  bucket;
  std::string  key;
  PutDataPath  path{PATH_NONE};
  std::int64_t created_at_ms{0};

  // 对齐 s3proxy 新增字段
  std::string   obj_id;              // = s3proxy ObjId
  std::uint64_t block_size{4194304}; // 4MB 固定
  std::uint64_t merged_size{0};      // UploadPart 累加，Complete 用作总大小
  std::int32_t  last_merged_part{0}; // 供未来续传/持久化
  std::int32_t  status{0};           // 0=进行中, 1=完成, 2=中止
  // 注：per-block crcs 存 PartRecord.block_crcs（有序），不在此处平铺累积。
};

/* 纯被动元数据存储接口，内存 mock 与 MongoDB 实现同一接口可无差别替换
 * 不含校验/etag 计算/client 比对，全在服务层 */
class IUploadIndex {
 public:
  virtual ~IUploadIndex() = default;

  /* 创建新会话，返回 upload_id（UUID） */
  [[nodiscard]] virtual std::string Create(
      const std::string& bucket, const std::string& key,
      PutDataPath path) = 0;

  /* 读会话，不存在返回 false；纯读不含业务判断 */
  [[nodiscard]] virtual bool Get(
      const std::string& upload_id, UploadRecord& out) = 0;

  /* 追加/覆盖 part（同 part_number 覆盖），不存在返回 false */
  [[nodiscard]] virtual bool AddPart(
      const std::string& upload_id, const PartRecord& part) = 0;

  /* 列出会话所有 part（未排序，排序/校验由服务层做） */
  [[nodiscard]] virtual bool ListParts(
      const std::string& upload_id, std::vector<PartRecord>& out) = 0;

  /* 删除会话（幂等） */
  virtual void Remove(const std::string& upload_id) = 0;

  /* 删除超过 ttl_ms 的过期会话（后台清理线程调用） */
  virtual void RemoveExpired(std::int64_t ttl_ms) = 0;

  // 增量写索引接口

  /* 更新已合并大小，对齐 s3proxy merged_size（Us3Turbo 无流式合并但保持兼容） */
  [[nodiscard]] virtual bool UpdateMergedSize(
      const std::string& upload_id,
      std::uint64_t merged_size) = 0;

  /* 更新最后合并 part 号，对齐 s3proxy last_merged_part_num */
  [[nodiscard]] virtual bool UpdateLastMergedPart(
      const std::string& upload_id,
      std::int32_t part_number) = 0;

  /* 写 fileidx_col 对象元数据，single_put 和 Complete 调用供 s3proxy 读取 */
  [[nodiscard]] virtual bool InsertFileIdx(
      const std::string& bucket,
      const std::string& key,
      const std::string& first_object,
      std::uint64_t block_size,
      std::uint64_t filesize,
      const std::string& hash) = 0;

  /* 读 fileidx_col 对象元数据，GetObject 第一步；未找到或失败均返回 false（调用方按 404 处理） */
  [[nodiscard]] virtual bool GetFileIdx(
      const std::string& bucket,
      const std::string& key,
      FileIdxRecord& out) = 0;
};

}  // namespace us3_turbo::proxy
