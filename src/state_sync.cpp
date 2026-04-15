#include "state_sync.h"
#include "net_utils.h"
#include "serializer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

static void throwErrno(const char *msg) {
  throw std::runtime_error(std::string(msg) + ": " + std::strerror(errno));
}

// Read exactly n bytes from fd within timeoutMs milliseconds.
// Returns false on EOF, error, or timeout (handles partial reads).
static bool readAll(int fd, uint8_t *buf, size_t n, int timeoutMs) {
  size_t got = 0;
  while (got < n) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = static_cast<long>(timeoutMs % 1000) * 1000L;
    if (::select(fd + 1, &fds, nullptr, nullptr, &tv) <= 0)
      return false; // timeout or error
    ssize_t k = ::read(fd, buf + got, n - got);
    if (k <= 0)
      return false; // EOF or error
    got += static_cast<size_t>(k);
  }
  return true;
}

// Write exactly n bytes to fd. Returns false on error.
static bool writeAll(int fd, const uint8_t *buf, size_t n) {
  size_t sent = 0;
  while (sent < n) {
    ssize_t k = ::write(fd, buf + sent, n - sent);
    if (k <= 0)
      return false;
    sent += static_cast<size_t>(k);
  }
  return true;
}

StateSync::StateSync(uint16_t tcpPort, StateProvider provider,
                     StateConsumer consumer)
    : tcpPort_(tcpPort), provider_(std::move(provider)),
      consumer_(std::move(consumer)) {}

StateSync::~StateSync() {
  if (serverRunning_.load())
    stopServer();
}

void StateSync::startServer() {
  if (serverRunning_.exchange(true))
    return;

  serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (serverFd_ < 0)
    throwErrno("StateSync::startServer: socket");

  int yes = 1;
  ::setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  sa.sin_port = htons(tcpPort_);

  if (::bind(serverFd_, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)) <
      0) {
    ::close(serverFd_);
    serverFd_ = -1;
    serverRunning_.store(false);
    throwErrno("StateSync::startServer: bind");
  }
  if (::listen(serverFd_, 8) < 0) {
    ::close(serverFd_);
    serverFd_ = -1;
    serverRunning_.store(false);
    throwErrno("StateSync::startServer: listen");
  }

  serverThread_ = std::thread(&StateSync::serverLoop, this);
}

void StateSync::stopServer() {
  if (!serverRunning_.exchange(false))
    return;
  // Interrupt the blocking accept() by shutting down and closing the socket.
  if (serverFd_ >= 0) {
    ::shutdown(serverFd_, SHUT_RDWR);
    ::close(serverFd_);
    serverFd_ = -1;
  }
  if (serverThread_.joinable())
    serverThread_.join();
}

void StateSync::serverLoop() {
  while (serverRunning_.load()) {
    if (serverFd_ < 0)
      break;

    // Use select with a short timeout so stop is responsive.
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(serverFd_, &fds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000; // 200 ms
    int ret = ::select(serverFd_ + 1, &fds, nullptr, nullptr, &tv);
    if (ret <= 0)
      continue; // timeout or interrupted

    int clientFd = ::accept(serverFd_, nullptr, nullptr);
    if (clientFd < 0)
      break; // socket closed — stop() was called
    handleClient(clientFd);
  }
}

void StateSync::handleClient(int clientFd) {
  // Read the one-byte STATE_REQUEST.
  uint8_t req = 0;
  if (!readAll(clientFd, &req, 1, 2000) || req != MSG_STATE_REQUEST) {
    ::close(clientFd);
    return;
  }

  // Obtain serialized state via the caller-supplied callback.
  std::vector<uint8_t> frame;
  try {
    frame = provider_();
  } catch (...) {
    ::close(clientFd);
    return;
  }

  writeAll(clientFd, frame.data(), frame.size());
  ::close(clientFd);
}

bool StateSync::requestState(const std::vector<std::string> &peers,
                             int timeoutPerPeerMs) {
  for (const auto &addr : peers) {
    if (tryPeer(addr, timeoutPerPeerMs))
      return true;
  }
  return false;
}

// Core client logic: connect to a single "ip:port" peer, send STATE_REQUEST,
// receive the full STATE frame, and deliver it to consumer.
static bool tryPeerImpl(const std::string &addr, uint16_t defaultPort,
                        const StateSync::StateConsumer &consumer,
                        int timeoutMs) {
  std::string ip;
  uint16_t port;
  parseAddr(addr, ip, port, defaultPort);

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return false;

  // Non-blocking connect so we can enforce a timeout.
  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip.c_str(), &sa.sin_addr) != 1) {
    ::close(fd);
    return false;
  }

  int ret = ::connect(fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa));
  if (ret < 0 && errno != EINPROGRESS) {
    ::close(fd);
    return false;
  }

  // Wait for connect to finish.
  {
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = static_cast<long>(timeoutMs % 1000) * 1000L;
    if (::select(fd + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
      ::close(fd);
      return false;
    }
    int err = 0;
    socklen_t errLen = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen);
    if (err != 0) {
      ::close(fd);
      return false;
    }
  }

  // Restore blocking mode.
  ::fcntl(fd, F_SETFL, flags);

  // Send STATE_REQUEST byte.
  static constexpr uint8_t kStateRequest = 0x01;
  if (!writeAll(fd, &kStateRequest, 1)) {
    ::close(fd);
    return false;
  }

  // Read the framing header: [1 B type][4 B length].
  uint8_t header[5];
  if (!readAll(fd, header, 5, timeoutMs)) {
    ::close(fd);
    return false;
  }
  if (header[0] != static_cast<uint8_t>(MsgType::STATE)) {
    ::close(fd);
    return false;
  }

  uint32_t payloadLen = (static_cast<uint32_t>(header[1]) << 24) |
                        (static_cast<uint32_t>(header[2]) << 16) |
                        (static_cast<uint32_t>(header[3]) << 8) |
                        static_cast<uint32_t>(header[4]);

  // Reconstruct the full frame (header + payload) for the consumer.
  std::vector<uint8_t> frame(5 + payloadLen);
  std::memcpy(frame.data(), header, 5);
  if (!readAll(fd, frame.data() + 5, payloadLen, timeoutMs)) {
    ::close(fd);
    return false; // server crashed mid-transfer
  }
  ::close(fd);

  // Hand the bytes to the caller-supplied consumer.
  try {
    consumer(frame);
  } catch (...) {
    return false;
  }
  return true;
}

bool StateSync::tryPeer(const std::string &addr, int timeoutMs) {
  return tryPeerImpl(addr, tcpPort_, consumer_, timeoutMs);
}

bool StateSync::requestState(const std::vector<std::string> &peers,
                             uint16_t tcpPort, const StateConsumer &consumer,
                             int timeoutPerPeerMs) {
  for (const auto &addr : peers) {
    if (tryPeerImpl(addr, tcpPort, consumer, timeoutPerPeerMs))
      return true;
  }
  return false;
}
