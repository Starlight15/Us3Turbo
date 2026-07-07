#include "proxy/src/storage/tcp_connection.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace us3_turbo::proxy {

TcpConnection::TcpConnection(const std::string& host, int port, int timeout_ms)
    : host_(host), port_(port), timeout_ms_(timeout_ms), fd_(-1) {
  std::string err;
  (void)Connect(err);  // 尝试连接，失败在首次 Send/Recv 时重试
}

TcpConnection::~TcpConnection() {
  Close();
}

bool TcpConnection::Connect(std::string& err) {
  if (fd_ >= 0) return true;

  fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ < 0) {
    err = std::string("socket() failed: ") + std::strerror(errno);
    return false;
  }

  // 设置收发超时
  struct timeval tv;
  tv.tv_sec = timeout_ms_ / 1000;
  tv.tv_usec = (timeout_ms_ % 1000) * 1000;
  setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port_));
  if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
    err = "invalid host: " + host_;
    Close();
    return false;
  }

  if (connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    err = std::string("connect() failed: ") + std::strerror(errno);
    Close();
    return false;
  }

  return true;
}

void TcpConnection::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool TcpConnection::SendAll(const void* data, std::size_t size, std::string& err) {
  if (!IsConnected() && !Connect(err)) {
    return false;
  }

  std::size_t sent = 0;
  const auto* p = static_cast<const char*>(data);
  while (sent < size) {
    ssize_t n = send(fd_, p + sent, size - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      err = std::string("send() failed: ") + std::strerror(errno);
      Close();  // 连接断开，下次重连
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool TcpConnection::RecvAll(void* data, std::size_t size, std::string& err) {
  if (!IsConnected()) {
    err = "connection not established";
    return false;
  }

  std::size_t received = 0;
  auto* p = static_cast<char*>(data);
  while (received < size) {
    ssize_t n = recv(fd_, p + received, size - received, MSG_WAITALL);
    if (n < 0) {
      if (errno == EINTR) continue;
      err = std::string("recv() failed: ") + std::strerror(errno);
      Close();
      return false;
    }
    if (n == 0) {
      err = "connection closed by peer";
      Close();
      return false;
    }
    received += static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace us3_turbo::proxy
