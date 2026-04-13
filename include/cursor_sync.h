#pragma once

#include "peer_socket.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// CursorSync: broadcasts and receives cursor positions over UDP.
//
// Wire format: [siteID : 4 bytes BE] [pos : 4 bytes signed BE] = 8 bytes.
//
// Each peer sends this packet whenever their cursor moves.  getRemoteCursors()
// returns a consistent snapshot of all currently known remote cursor positions
// keyed by siteID.
//
// setOnUpdate() registers a callback fired (from the receive thread) on every
// incoming cursor update — use it to post a render event to the UI loop.
// Must be called before start().
class CursorSync {
public:
  explicit CursorSync(uint16_t port, uint32_t siteID);
  ~CursorSync();

  CursorSync(const CursorSync &) = delete;
  CursorSync &operator=(const CursorSync &) = delete;

  void addPeer(const std::string &addr); // "ip:cursorPort"
  void removePeer(uint32_t siteID);      // call when a peer leaves

  // Callback fired on every incoming cursor packet (runs on recv thread).
  void setOnUpdate(std::function<void()> cb);

  void start();
  void stop();

  // Send our current cursor position to all registered peers.
  void broadcast(int pos);

  // Thread-safe snapshot: siteID → document byte offset.
  std::unordered_map<uint32_t, int> getRemoteCursors() const;

private:
  void recvLoop();

  PeerSocket socket_;
  uint32_t siteID_;

  mutable std::mutex mu_;
  std::unordered_map<uint32_t, int> remoteCursors_;
  std::function<void()> onUpdate_;

  std::thread recvThread_;
  std::atomic<bool> running_{false};
};
