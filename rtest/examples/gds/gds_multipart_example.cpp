// gds_multipart_example.cpp — GDS 分段上传端到端验证。
//
// client → proxy(CreateMultipartUpload / UploadPartGds /
// CompleteMultipartUpload)
//        → backend(PutBlock，按 source_offset 反向 RDMA-READ 各 block)。
// 每个 part 独立注册 cuObj RDMA token（避免单 token 1GB 上限），proxy 内部
// 把 part 切成 4MB block 并发拉取。
//
// 用法：
//   us3_turbo_gds_multipart_example \
//     --proxy 192.168.1.198:9100 \
//     --part-size 4M --num-parts 4 [--verify-crc32c]
//
// 注意：非 last part 必须恰好等于 proxy 的 multipart_part_size（默认 4MB），
// 仅 last part 可小于此值。违反将在 Complete 时被 proxy 拒绝。
//
// 模型：进程内共享一个 Client；单个 GPU buffer 复用上传 num-parts 次。

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

#include <cuda_runtime.h>

namespace {

using clk = std::chrono::steady_clock;
using ms_double = std::chrono::duration<double, std::milli>;

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  std::string proxy_addr = "192.168.1.198:9100";
  std::uint64_t part_size = 4ULL * 1024 * 1024;  // 默认 4MiB（须与 proxy 一致）
  std::uint32_t num_parts = 4;
  bool verify = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need = [&](std::string& v) -> bool {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << arg << "\n";
        return false;
      }
      v = argv[++i];
      return true;
    };
    if (arg == "--proxy") {
      if (!need(proxy_addr)) return 2;
    } else if (arg == "--part-size") {
      std::string v;
      if (!need(v) || !rtest::ParseSize(v, part_size)) {
        std::cerr << "bad --part-size\n";
        return 2;
      }
    } else if (arg == "--num-parts") {
      std::string v;
      if (!need(v)) return 2;
      num_parts = static_cast<std::uint32_t>(std::strtoull(v.c_str(), nullptr, 10));
      if (num_parts == 0) {
        std::cerr << "bad --num-parts\n";
        return 2;
      }
    } else if (arg == "--verify-crc32c") {
      verify = true;
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  const std::uint64_t total = part_size * num_parts;
  std::cout << "=== GDS multipart ===\n"
            << "  proxy     : " << proxy_addr << "\n"
            << "  part size : " << rtest::HumanBytes(part_size) << "\n"
            << "  num parts : " << num_parts << "\n"
            << "  total     : " << rtest::HumanBytes(total) << "\n"
            << "  verify-crc: " << (verify ? "on" : "off") << "\n"
            << std::endl;

  // GPU buffer（复用上传 num_parts 次）。
  void* dev = nullptr;
  cudaError_t e = cudaMalloc(&dev, part_size);
  if (e != cudaSuccess) {
    std::cerr << "cudaMalloc: " << cudaGetErrorString(e) << "\n";
    return 1;
  }
  std::vector<std::byte> host(part_size);
  rtest::FillHostPattern(host);
  e = cudaMemcpy(dev, host.data(), part_size, cudaMemcpyHostToDevice);
  if (e != cudaSuccess) {
    std::cerr << "cudaMemcpy: " << cudaGetErrorString(e) << "\n";
    cudaFree(dev);
    return 1;
  }

  ClientOptions opts;
  opts.endpoint = proxy_addr;
  opts.verify_crc32c = verify;

  Client client(std::move(opts));
  if (!client.Initialize()) {
    std::cerr << "Initialize failed\n";
    cudaFree(dev);
    return 1;
  }

  std::string upload_id, error;
  if (!client.CreateMultipartUpload("test-bucket", "gds-multipart.dat", PutDataPath::kGds,
                                    upload_id, error)) {
    std::cerr << "CreateMultipartUpload failed: " << error << "\n";
    cudaFree(dev);
    return 1;
  }
  std::cout << "CreateMultipartUpload: upload_id=" << upload_id << "\n";

  std::vector<Client::PartInfo> parts;
  parts.reserve(num_parts);

  const auto t0 = clk::now();
  for (std::uint32_t i = 1; i <= num_parts; ++i) {
    std::string etag;
    if (!client.UploadPartGds(upload_id, i, ConstBufferView{.data = dev, .size = part_size}, etag,
                              error)) {
      std::cerr << "UploadPartGds " << i << " failed: " << error << "\n";
      cudaFree(dev);
      return 1;
    }
    std::cout << "UploadPartGds part " << i << " etag=" << etag << "\n";
    parts.push_back({i, etag});
  }

  Client::CompletedMultipart done;
  if (!client.CompleteMultipartUpload(upload_id, parts, done)) {
    std::cerr << "CompleteMultipartUpload failed: " << done.error << "\n";
    cudaFree(dev);
    return 1;
  }
  const auto t1 = clk::now();
  const double wall_ms = ms_double(t1 - t0).count();

  std::cout << "CompleteMultipartUpload: object_id=" << done.object_id
            << " size=" << done.object_size << " etag=" << done.etag << "\n";
  const double wall_s = wall_ms / 1000.0;
  const double mbs = (wall_s > 0.0) ? static_cast<double>(total) / wall_s / (1024.0 * 1024.0) : 0.0;
  std::cout << "wall=" << wall_ms << "ms throughput=" << mbs << " MiB/s\n";

  client.Shutdown();
  cudaFree(dev);

  if (done.object_size != total) {
    std::cerr << "size mismatch: got " << done.object_size << " want " << total << "\n";
    return 1;
  }
  return 0;
}
