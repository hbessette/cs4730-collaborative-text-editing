#pragma once

// PeerManager: peer discovery and liveness tracking via periodic heartbeats.
//
// Each instance has a unique uint32_t siteID (generated at startup).
// A background thread sends a HEARTBEAT to every known peer every 2 s.
// A peer is considered offline after 6 s of silence (3 missed heartbeats).
//
// Bootstrap: call addKnownPeer("ip:port") with at least one reachable peer
// before start().  Auto-discovery is bidirectional: once a remote peer
// receives a message from this node it adds us back automatically.
//
// Wire format for all heartbeat messages (7 bytes):
//   [1 byte : MsgType  (0x01=JOIN, 0x02=LEAVE, 0x03=HEARTBEAT)]
//   [4 bytes: siteID,       big-endian uint32]
//   [2 bytes: listen port,  big-endian uint16]

#include "peer_socket.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Generate a random uint32 site ID (called once at program startup).
uint32_t generateSiteID();

enum class HBMsgType : uint8_t { JOIN = 0x01, LEAVE = 0x02, HEARTBEAT = 0x03 };

class PeerManager {
public:
  // Callback signature: (siteID, "ip:port")
  using PeerCallback =
      std::function<void(uint32_t siteID, const std::string &addr)>;

  // heartbeatPort: UDP port this manager listens on (default 5001).
  // siteID: unique identifier for this node (use generateSiteID()).
  explicit PeerManager(uint16_t heartbeatPort = 5001,
                       uint32_t siteID = generateSiteID());
  ~PeerManager();

  PeerManager(const PeerManager &) = delete;
  PeerManager &operator=(const PeerManager &) = delete;

  // Register callback for when a new peer is first seen.
  void setOnPeerJoin(PeerCallback cb);

  // Register callback for when a peer is declared offline or sends LEAVE.
  void setOnPeerLeave(PeerCallback cb);

  // Pre-seed a known peer address ("ip:port") before calling start().
  // Can also be called after start() to introduce a new peer.
  void addKnownPeer(const std::string &addr);

  // Broadcast JOIN and start the background heartbeat/receive thread.
  void start();

  // Broadcast LEAVE and stop the background thread.  Called automatically
  // by the destructor if still running.
  void stop();

  bool isRunning() const { return running_.load(); }

  // Snapshot of currently active peers { siteID → "ip:port" }.
  std::vector<std::pair<uint32_t, std::string>> activePeers() const;

  uint32_t siteID() const { return siteID_; }

private:
  struct PeerInfo {
    std::string addr;
    std::chrono::steady_clock::time_point lastSeen;
  };

  void run();
  void sendMsg(HBMsgType type);
  void processMsg(const std::vector<uint8_t> &data, const std::string &addr);
  void checkDeadPeers();

  PeerSocket socket_;
  uint32_t siteID_;

  mutable std::mutex mutex_;
  std::map<uint32_t, PeerInfo> peers_; // siteID → info

  std::thread thread_;
  std::atomic<bool> running_{false};

  PeerCallback joinCb_;
  PeerCallback leaveCb_;

  static constexpr int HEARTBEAT_INTERVAL_MS = 2000;
  static constexpr int DEAD_THRESHOLD_MS = 6000;
};
