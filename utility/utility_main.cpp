// utility_main.cpp — 对象存储命令行工具:上传 / 下载指定对象。
//
// 取代原 rtest/examples/ 的 6 个教学示例,合并为一个可实际使用的工具:
//   upload   把本地文件上传为对象 (size <= 4M 走单步 PUT,否则分段上传)
//   download 把对象下载到本地文件
// 数据通路 --path rdma(host 内存,默认)/ gds(device 显存,仅 GDS 编译开启时可用)。
//
// 用法:
//   us3_turbo_utility upload   --file <local> --bucket B --key K [opts]
//   us3_turbo_utility download --file <local> --bucket B --key K [opts]
//
// 示例:
//   us3_turbo_utility upload   --file /data/a.bin --bucket test-bucket \
//       --key a.bin --path rdma --proxy 10.72.142.155:9100 --rdma-bind-ip 10.72.142.155
//   us3_turbo_utility download --file /tmp/a.bin --bucket test-bucket \
//       --key a.bin --path rdma --proxy 10.72.142.155:9100 --rdma-bind-ip 10.72.142.155

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

#ifdef US3_TURBO_ACCESS_ENABLE_GDS
#include <cuda_runtime.h>
#endif

using us3_turbo::client::Client;
using us3_turbo::client::ClientOptions;
using us3_turbo::client::ClientProxyPutRequest;
using us3_turbo::client::ClientProxyPutResponse;
using us3_turbo::client::ConstBufferView;
using us3_turbo::client::GetPathResult;
using us3_turbo::client::MutableBufferView;
using us3_turbo::client::PutDataPath;

namespace {

constexpr const char* kTool = "us3_turbo_utility";
// 单步 PUT 上限,须与 proxy --max_single_put_bytes / ClientOptions::put_single_max_bytes 一致。
constexpr std::uint64_t kMaxSinglePutBytes = 4ULL * 1024 * 1024;

struct Options {
  std::string command;                    // "upload" | "download"
  std::string path{"rdma"};               // "rdma" | "gds"
  std::string proxy{rtest::kDefaultProxyEndpoint};
  std::string rdma_bind_ip;               // --path rdma 时必填
  std::string bucket{"test-bucket"};
  std::string key;
  std::string file;                       // 本地文件路径
  std::uint64_t part_size{rtest::kDefaultPartSize};
};

void PrintUsage() {
  std::cout
      << "用法: " << kTool << " <upload|download> [options]\n"
      << "\n"
      << "  upload   : 把本地文件上传为对象 (size <= 4M 单步 PUT,否则分段上传)\n"
      << "  download : 把对象下载到本地文件\n"
      << "\n"
      << "options:\n"
      << "  --file PATH           本地文件路径 (upload 读源 / download 写目标)\n"
      << "  --bucket NAME         bucket (默认 test-bucket)\n"
      << "  --key KEY             对象 key\n"
      << "  --path rdma|gds       数据通路 (默认 rdma;gds 仅 GDS 编译开启时可用)\n"
      << "  --proxy HOST:PORT     proxy endpoint (默认 " << rtest::kDefaultProxyEndpoint << ")\n"
      << "  --rdma-bind-ip IP     RDMA 通路绑定 IP (--path rdma 必填)\n"
      << "  --part-size SIZE      分段上传 part 大小 (默认 8M,须与 proxy --multipart_part_size 一致)\n"
      << "\n"
      << "示例:\n"
      << "  " << kTool << " upload   --file /data/a.bin --key a.bin --path rdma --rdma-bind-ip 10.72.142.155\n"
      << "  " << kTool << " download --file /tmp/a.bin --key a.bin --path rdma --rdma-bind-ip 10.72.142.155\n";
}

bool NeedVal(int& i, int argc, char** argv, std::string_view arg, std::string_view& val) {
  if (i + 1 >= argc) {
    std::cerr << "missing value for " << arg << "\n";
    return false;
  }
  val = argv[++i];
  return true;
}

bool ParseArgs(int argc, char** argv, Options& o) {
  if (argc < 2) {
    PrintUsage();
    return false;
  }
  o.command = argv[1];
  if (o.command != "upload" && o.command != "download") {
    std::cerr << "unknown command: " << o.command << "\n";
    PrintUsage();
    return false;
  }
  for (int i = 2; i < argc; ++i) {
    std::string_view arg = argv[i];
    std::string_view val;
    auto need = [&] { return NeedVal(i, argc, argv, arg, val); };
    if (arg == "--file") {
      if (!need()) return false;
      o.file = std::string(val);
    } else if (arg == "--bucket") {
      if (!need()) return false;
      o.bucket = std::string(val);
    } else if (arg == "--key") {
      if (!need()) return false;
      o.key = std::string(val);
    } else if (arg == "--path") {
      if (!need()) return false;
      o.path = std::string(val);
    } else if (arg == "--proxy") {
      if (!need()) return false;
      o.proxy = std::string(val);
    } else if (arg == "--rdma-bind-ip") {
      if (!need()) return false;
      o.rdma_bind_ip = std::string(val);
    } else if (arg == "--part-size") {
      if (!need() || !rtest::ParseSize(val, o.part_size)) {
        std::cerr << "bad --part-size\n";
        return false;
      }
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return false;
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      PrintUsage();
      return false;
    }
  }
  if (o.file.empty()) {
    std::cerr << "--file is required\n";
    return false;
  }
  if (o.key.empty()) {
    std::cerr << "--key is required\n";
    return false;
  }
  if (o.path != "rdma" && o.path != "gds") {
    std::cerr << "--path must be rdma or gds\n";
    return false;
  }
  return true;
}

PutDataPath ResolvePath(const Options& o) {
  return (o.path == "gds") ? PutDataPath::kGds : PutDataPath::kRdma;
}

bool ReadFile(const std::string& path, std::vector<std::byte>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "open failed: " << path << "\n";
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamsize n = in.tellg();
  if (n <= 0) {
    std::cerr << "empty or unreadable file: " << path << "\n";
    return false;
  }
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<std::size_t>(n));
  in.read(reinterpret_cast<char*>(out.data()), n);
  if (!in) {
    std::cerr << "read failed: " << path << "\n";
    return false;
  }
  return true;
}

bool WriteFile(const std::string& path, const void* data, std::size_t size) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::cerr << "open failed for write: " << path << "\n";
    return false;
  }
  out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  out.close();
  if (!out) {
    std::cerr << "write failed: " << path << "\n";
    return false;
  }
  return true;
}

// 单步 PUT 上传 (size <= kMaxSinglePutBytes)。
bool UploadSingle(Client& client, const Options& o, const std::vector<std::byte>& host,
                  std::string& out_etag) {
  const std::uint64_t size = host.size();
  ClientProxyPutRequest req;
  req.bucket = o.bucket;
  req.key = o.key;
  req.object_size = size;
  req.path = ResolvePath(o);

  ClientProxyPutResponse resp;
  bool ok = false;
  if (o.path == "gds") {
#ifdef US3_TURBO_ACCESS_ENABLE_GDS
    void* dev = nullptr;
    if (cudaMalloc(&dev, size) != cudaSuccess) {
      std::cerr << "cudaMalloc failed\n";
      return false;
    }
    cudaMemcpy(dev, host.data(), size, cudaMemcpyHostToDevice);
    ok = client.PutObjectGds(req, ConstBufferView{.data = dev, .size = size}, resp);
    if (ok && resp.gds_result) out_etag = resp.gds_result->etag;
    cudaFree(dev);
#else
    std::cerr << "GDS path not compiled (rebuild with --enable-gds)\n";
    return false;
#endif
  } else {
    ok = client.PutObjectRdma(req, ConstBufferView{.data = host.data(), .size = size}, resp);
    if (ok && resp.rdma_result) out_etag = resp.rdma_result->etag;
  }
  return ok;
}

// 分段上传 (size > kMaxSinglePutBytes)。
bool UploadMultipart(Client& client, const Options& o, const std::vector<std::byte>& host,
                     std::string& out_etag) {
  const std::uint64_t size = host.size();
  std::string upload_id, trace_id, err;
  if (!client.CreateMultipartUpload(o.bucket, o.key, ResolvePath(o), upload_id, trace_id, err)) {
    std::cerr << "CreateMultipartUpload failed: " << err << "\n";
    return false;
  }

  const std::uint32_t num_parts =
      static_cast<std::uint32_t>((size + o.part_size - 1) / o.part_size);
  std::vector<Client::PartInfo> parts;
  parts.reserve(num_parts);

  // GDS 复用一块 part 大小的 device buffer,逐 part 拷贝。
#ifdef US3_TURBO_ACCESS_ENABLE_GDS
  void* dev = nullptr;
  if (o.path == "gds" && cudaMalloc(&dev, o.part_size) != cudaSuccess) {
    std::cerr << "cudaMalloc failed\n";
    (void)client.AbortMultipartUpload(upload_id, err);
    return false;
  }
#endif

  bool failed = false;
  for (std::uint32_t i = 1; i <= num_parts; ++i) {
    const std::uint64_t off = static_cast<std::uint64_t>(i - 1) * o.part_size;
    const std::uint64_t len = std::min<std::uint64_t>(o.part_size, size - off);
    std::string part_etag;
    bool pok = false;
    if (o.path == "gds") {
#ifdef US3_TURBO_ACCESS_ENABLE_GDS
      cudaMemcpy(dev, host.data() + off, len, cudaMemcpyHostToDevice);
      pok = client.UploadPartGds(upload_id, i, ConstBufferView{.data = dev, .size = len},
                                 part_etag, err);
#else
      std::cerr << "GDS path not compiled (rebuild with --enable-gds)\n";
      failed = true;
      break;
#endif
    } else {
      pok = client.UploadPartRdma(upload_id, i,
                                  ConstBufferView{.data = host.data() + off, .size = len},
                                  part_etag, err);
    }
    if (!pok) {
      std::cerr << "UploadPart " << i << " failed: " << err << "\n";
      failed = true;
      break;
    }
    parts.push_back({i, part_etag});
  }

#ifdef US3_TURBO_ACCESS_ENABLE_GDS
  if (dev != nullptr) cudaFree(dev);
#endif

  if (failed) {
    (void)client.AbortMultipartUpload(upload_id, err);
    return false;
  }

  Client::CompletedMultipart done;
  if (!client.CompleteMultipartUpload(upload_id, parts, done)) {
    std::cerr << "CompleteMultipartUpload failed: " << done.error << "\n";
    return false;
  }
  out_etag = done.etag;
  return done.object_size == size;
}

int DoUpload(const Options& o) {
  std::vector<std::byte> host;
  if (!ReadFile(o.file, host)) return 1;
  const std::uint64_t size = host.size();

  Client client(ClientOptions{.endpoint = o.proxy,
                              .multipart_part_size = o.part_size,
                              .rdma_bind_ip = o.rdma_bind_ip});
  if (!client.Initialize()) {
    std::cerr << "Client::Initialize failed\n";
    return 1;
  }

  std::string etag;
  bool ok = (size <= kMaxSinglePutBytes) ? UploadSingle(client, o, host, etag)
                                         : UploadMultipart(client, o, host, etag);
  client.Shutdown();

  if (ok) {
    std::cout << "OK upload bucket=" << o.bucket << " key=" << o.key
              << " size=" << rtest::HumanBytes(size) << " etag=" << etag << "\n";
    return 0;
  }
  std::cerr << "upload FAILED\n";
  return 1;
}

int DoDownload(const Options& o) {
  Client client(ClientOptions{.endpoint = o.proxy, .rdma_bind_ip = o.rdma_bind_ip});
  if (!client.Initialize()) {
    std::cerr << "Client::Initialize failed\n";
    return 1;
  }

  std::uint64_t size = 0;
  std::string trace_id, err;
  if (!client.StatObject(o.bucket, o.key, size, trace_id, err)) {
    std::cerr << "StatObject failed: " << err << "\n";
    client.Shutdown();
    return 1;
  }
  std::cout << "StatObject size=" << rtest::HumanBytes(size) << "\n";

  // 空对象:直接落一个空文件,跳过数据面。
  if (size == 0) {
    client.Shutdown();
    std::vector<std::byte> empty;
    if (!WriteFile(o.file, empty.data(), 0)) return 1;
    std::cout << "OK download bucket=" << o.bucket << " key=" << o.key << " size=0 B\n";
    return 0;
  }

  GetPathResult res;
  bool ok = false;
  if (o.path == "gds") {
#ifdef US3_TURBO_ACCESS_ENABLE_GDS
    void* dev = nullptr;
    if (cudaMalloc(&dev, size) != cudaSuccess) {
      std::cerr << "cudaMalloc failed\n";
      client.Shutdown();
      return 1;
    }
    ok = client.GetObjectGds(o.bucket, o.key, MutableBufferView{.data = dev, .size = size}, res);
    if (ok && res.ok) {
      std::vector<std::byte> host(size);
      cudaMemcpy(host.data(), dev, size, cudaMemcpyDeviceToHost);
      cudaFree(dev);
      ok = WriteFile(o.file, host.data(), size);
    } else {
      cudaFree(dev);
    }
#else
    std::cerr << "GDS path not compiled (rebuild with --enable-gds)\n";
    client.Shutdown();
    return 1;
#endif
  } else {
    std::vector<std::byte> host(size);
    ok = client.GetObjectRdma(o.bucket, o.key, MutableBufferView{.data = host.data(), .size = size},
                              res);
    if (ok && res.ok) ok = WriteFile(o.file, host.data(), size);
  }

  client.Shutdown();
  if (ok && res.ok) {
    std::cout << "OK download bucket=" << o.bucket << " key=" << o.key
              << " size=" << rtest::HumanBytes(size) << " bytes_read=" << res.bytes_read
              << " crc32c=0x" << std::hex << res.crc32c << std::dec << "\n";
    return 0;
  }
  std::cerr << "download FAILED: " << res.error_message << "\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!ParseArgs(argc, argv, o)) return 2;
  if (o.command == "upload") return DoUpload(o);
  return DoDownload(o);
}
