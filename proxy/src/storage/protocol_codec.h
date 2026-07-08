#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "proxy/src/storage/ufile_ac_protocol.h"

namespace us3_turbo::proxy::codec {

// 编码 GDS PUT 请求到 out_buffer，返回总字节数。
// buffer = Message(52) + GdsPutReq(52) + key + rdma_token
std::size_t EncodeGdsPutRequest(
    const std::string& key,
    const std::string& rdma_token,
    std::uint64_t gpu_offset,
    std::uint64_t data_len,
    std::uint32_t setid,
    std::uint64_t session_id,
    std::vector<char>& out_buffer);

// 解码 GDS PUT 响应体（GdsPutRsp + errmsg，不含 Message 头）。
// 返回 0=成功（out_err 填 backend 的 errmsg，可能空）；
//        -1=格式错误（out_err 填错误描述）。
int DecodeGdsPutResponse(
    const char* buffer,
    std::size_t len,
    GdsPutRsp& out_rsp,
    std::string& out_err);

// 编码 UCX PUT 请求。
// buffer = Message(52) + UcxPutReq(68) + key + client_ucx_addr + packed_rkey
std::size_t EncodeUcxPutRequest(
    const std::string& key,
    std::uint64_t remote_addr,
    const std::string& packed_rkey,
    const std::string& client_ucx_addr,
    std::uint64_t source_offset,
    std::uint64_t data_len,
    std::uint32_t setid,
    std::uint64_t session_id,
    std::vector<char>& out_buffer);

// 解码 UCX PUT 响应体（UcxPutRsp + errmsg）。
// 返回 0=成功（out_err 填 backend errmsg），-1=格式错误。
int DecodeUcxPutResponse(
    const char* buffer,
    std::size_t len,
    UcxPutRsp& out_rsp,
    std::string& out_err);

}  // namespace us3_turbo::proxy::codec
