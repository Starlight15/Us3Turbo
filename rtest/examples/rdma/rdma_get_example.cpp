// rdma_get_example.cpp — RDMA (libibverbs) PUT + GET 读回验证。
//
// 演示: 用 RdmaPutChannel PUT 一个对象, 再用 RdmaGetChannel GET 读回并逐字节比对。
// host 内存操作, 无 CUDA 依赖, 仅依赖 libibverbs + librdmacm。

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/common/request.h"
#include "rtest/common.h"
#include "us3_turbo/client/client.h"

int main(int argc, char** argv) {
  using namespace us3_turbo::client;

  const char* kProxy = (argc > 1) ? argv[1] : "192.168.1.198:9100";
  constexpr const char* kBucket = "test-bucket";
  constexpr std::uint64_t kSize = 4ULL * 1024 * 1024;  // 4 MiB

  /* PUT buffer — 用确定性 pattern 填充。 */
  std::vector<std::byte> host_put(kSize);
  rtest::FillHostPattern(host_put);

  /* GET buffer — 初始化为 0xAA 以检测无效写入。 */
  std::vector<std::byte> host_get(kSize);
  std::memset(host_get.data(), 0xAA, kSize);

  Client client(ClientOptions{.endpoint = kProxy});
  if (!client.Initialize()) {
    std::cerr << "Client::Initialize failed\n";
    return 1;
  }

  /* ---- PUT ---- */
  ClientProxyPutResponse put_resp;
  bool put_ok = client.PutObjectRdma(
      ClientProxyPutRequest{.bucket = kBucket,
                            .key = "rdma-get-demo",
                            .object_size = kSize,
                            .path = PutDataPath::kRdma},
      ConstBufferView{.data = host_put.data(), .size = kSize}, put_resp);
  if (!put_ok) {
    std::cerr << "PUT failed\n";
    client.Shutdown();
    return 1;
  }
  std::cout << "PUT etag=" << put_resp.rdma_result.value().etag << "\n";

  /* ---- StatObject ---- */
  std::uint64_t obj_size = 0;
  std::string stat_err;
  if (!client.StatObject(kBucket, "rdma-get-demo", obj_size, stat_err)) {
    std::cerr << "StatObject failed: " << stat_err << "\n";
    client.Shutdown();
    return 1;
  }
  std::cout << "StatObject size=" << rtest::HumanBytes(obj_size) << "\n";

  /* ---- GET ---- */
  GetPathResult get_res;
  bool get_ok = client.GetObjectRdma(
      kBucket, "rdma-get-demo",
      MutableBufferView{.data = host_get.data(), .size = obj_size}, get_res);
  if (!get_ok) {
    std::cerr << "GET failed: " << get_res.error_message << "\n";
    client.Shutdown();
    return 1;
  }
  std::cout << "GET bytes_read=" << get_res.bytes_read
            << " crc32c=0x" << std::hex << get_res.crc32c << std::dec
            << " hash=" << get_res.hash << "\n";

  /* ---- 逐字节比对 ---- */
  bool ok = rtest::VerifyHostBuffer(host_get.data(), kSize, host_put, "rdma-get-demo");

  client.Shutdown();
  return ok ? 0 : 1;
}
