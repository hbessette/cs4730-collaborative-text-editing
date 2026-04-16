#pragma once

#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <utility>
#include <vector>

class PeerSocket {
public:
  explicit PeerSocket(uint16_t port = 10000);
  ~PeerSocket();

  PeerSocket(const PeerSocket &) = delete;
  PeerSocket &operator=(const PeerSocket &) = delete;

  // Register a peer address in "ip:port" or "ip" (uses own port) form.
  void addPeer(const std::string &addr);

  // Send data to all registered peers.
  void send(const std::vector<uint8_t> &data);

  // Block until a datagram arrives (or timeout elapses).
  //   timeout_ms < 0 → block indefinitely
  //   timeout_ms >= 0 → wait up to that many milliseconds, then throw
  //                     std::runtime_error("timeout")
  // Returns { payload, "sender_ip:port" }.
  std::pair<std::vector<uint8_t>, std::string> receive(int timeout_ms = -1);

  int fd() const { return sock_; }

private:
  int sock_;
  uint16_t port_;
  std::vector<struct sockaddr_in> peers_;
};
