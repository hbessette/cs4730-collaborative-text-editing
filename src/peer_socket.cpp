#include "peer_socket.h"
#include "net_utils.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

static void throwErrno(const char *msg) {
  throw std::runtime_error(std::string(msg) + ": " + std::strerror(errno));
}

PeerSocket::PeerSocket(uint16_t port) : sock_(-1), port_(port) {
  sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sock_ < 0)
    throwErrno("PeerSocket: socket");

  int yes = 1;
  if (::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
    ::close(sock_);
    throwErrno("PeerSocket: SO_REUSEADDR");
  }

#ifdef SO_REUSEPORT
  if (::setsockopt(sock_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) < 0) {
    ::close(sock_);
    throwErrno("PeerSocket: SO_REUSEPORT");
  }
#endif

  struct sockaddr_in bindAddr;
  std::memset(&bindAddr, 0, sizeof(bindAddr));
  bindAddr.sin_family = AF_INET;
  bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  bindAddr.sin_port = htons(port_);

  if (::bind(sock_, reinterpret_cast<struct sockaddr *>(&bindAddr),
             sizeof(bindAddr)) < 0) {
    ::close(sock_);
    throwErrno("PeerSocket: bind");
  }
}

PeerSocket::~PeerSocket() {
  if (sock_ >= 0)
    ::close(sock_);
}

// Parse "ip:port" or bare "ip" (uses own port_).
void PeerSocket::addPeer(const std::string &addr) {
  struct sockaddr_in sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;

  std::string ip;
  uint16_t port = port_;
  parseAddr(addr, ip, port, port_);

  if (::inet_pton(AF_INET, ip.c_str(), &sa.sin_addr) != 1)
    throw std::runtime_error("PeerSocket::addPeer: invalid address: " + addr);

  sa.sin_port = htons(port);
  peers_.push_back(sa);
}

void PeerSocket::send(const std::vector<uint8_t> &data) {
  const void *buf = data.empty() ? static_cast<const void *>("") : data.data();
  for (const auto &peer : peers_) {
    ::sendto(sock_, buf, data.size(), 0,
             reinterpret_cast<const struct sockaddr *>(&peer), sizeof(peer));
  }
}

std::pair<std::vector<uint8_t>, std::string>
PeerSocket::receive(int timeout_ms) {
  if (timeout_ms >= 0) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock_, &fds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = static_cast<long>(timeout_ms % 1000) * 1000L;

    int ret = ::select(sock_ + 1, &fds, nullptr, nullptr, &tv);
    if (ret == 0)
      throw std::runtime_error("PeerSocket::receive: timeout");
    if (ret < 0)
      throwErrno("PeerSocket::receive: select");
  }

  uint8_t buf[65536];
  struct sockaddr_in sender;
  socklen_t senderLen = sizeof(sender);

  ssize_t n =
      ::recvfrom(sock_, buf, sizeof(buf), 0,
                 reinterpret_cast<struct sockaddr *>(&sender), &senderLen);
  if (n < 0)
    throwErrno("PeerSocket::receive: recvfrom");

  char ipStr[INET_ADDRSTRLEN];
  ::inet_ntop(AF_INET, &sender.sin_addr, ipStr, sizeof(ipStr));
  std::string senderAddr =
      std::string(ipStr) + ":" + std::to_string(ntohs(sender.sin_port));

  return {std::vector<uint8_t>(buf, buf + n), senderAddr};
}
