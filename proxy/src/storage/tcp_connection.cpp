#include "proxy/src/storage/tcp_connection.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace us3_turbo::proxy {

TcpConnection::TcpConnection(std::string host, int port, int timeout_ms)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms), fd_(-1) {}

TcpConnection::~TcpConnection() { Close(); }

bool TcpConnection::Connect() {
  if (alive_.load(std::memory_order_acquire)) return true;
  if (fd_ >= 0) Close();  // 清理残留 fd

  fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ < 0) return false;

  struct timeval tv;
  tv.tv_sec  = timeout_ms_ / 1000;
  tv.tv_usec = (timeout_ms_ % 1000) * 1000;
  ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(static_cast<std::uint16_t>(port_));
  if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
    Close();
    return false;
  }
  if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    Close();
    return false;
  }
  alive_.store(true, std::memory_order_release);
  return true;
}

void TcpConnection::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  alive_.store(false, std::memory_order_release);
}

int TcpConnection::SendAll(const void* buf, std::size_t len) {
  if (!alive_.load(std::memory_order_acquire)) return -1;  // 须先经 AcquireConn->Connect
  std::size_t sent = 0;
  const auto* p = static_cast<const char*>(buf);
  while (sent < len) {
    ssize_t n = ::send(fd_, p + sent, len - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      set_dead();
      Close();
      return -1;
    }
    sent += static_cast<std::size_t>(n);
  }
  return 0;
}

int TcpConnection::RecvAll(void* buf, std::size_t len) {
  if (!alive_.load(std::memory_order_acquire)) return -1;
  std::size_t got = 0;
  auto* p = static_cast<char*>(buf);
  while (got < len) {
    ssize_t n = ::recv(fd_, p + got, len - got, 0);  // 不用 MSG_WAITALL：超时按“无数据”计而非“未收满”，慢速对端不误杀
    if (n < 0) {
      if (errno == EINTR) continue;
      set_dead();
      Close();
      return -1;
    }
    if (n == 0) {  // 对端关闭
      set_dead();
      Close();
      return -1;
    }
    got += static_cast<std::size_t>(n);
  }
  return 0;
}

}  // namespace us3_turbo::proxy
