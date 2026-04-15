#pragma once

// StateSync: TCP-based full-state transfer for late-joining peers.
//
// Server role
//   A running peer calls startServer() to accept incoming STATE_REQUEST
//   connections. When a request arrives, it invokes the StateProvider callback
//   (which serializes the CRDT under whatever lock the caller chooses) and
//   sends the raw bytes back over TCP before closing the connection.
//
// Client role
//   A new (or recovering) peer calls requestState(peers, timeout).  Each
//   address is tried in order; for each one a TCP connection is opened,
//   a STATE_REQUEST byte is sent, and the full STATE frame is read back.
//   On success the received bytes are passed to the StateConsumer callback
//   (which decodes and applies the state).  If the server crashes mid-transfer
//   (partial read, EOF, or timeout) the next peer is tried automatically.
//
// Wire format
//   Request  (client → server): 1 byte = 0x01 (STATE_REQUEST)
//   Response (server → client): Serializer::encodeState() frame
//                               [1 B : MsgType::STATE (0x02)]
//                               [4 B : payload length, big-endian uint32]
//                               [N B : payload]

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class StateSync {
public:
  // StateProvider: called by the server thread to obtain serialized state
  // bytes. Must be thread-safe (the server thread calls it concurrently with
  // the UI).
  using StateProvider = std::function<std::vector<uint8_t>()>;

  // StateConsumer: called by requestState() on the thread that called it,
  // after a full STATE frame has been received successfully.
  // Must apply the bytes to the local CRDT (e.g. via Serializer::decodeState
  // followed by CRDTEngine::loadState).  Throw on malformed data.
  using StateConsumer = std::function<void(const std::vector<uint8_t> &)>;

  // tcpPort:  TCP port this instance listens on (server) / connects to
  // (client). provider: serialize-state callback, used by the server thread.
  // consumer: apply-state callback, called by requestState() on success.
  explicit StateSync(uint16_t tcpPort, StateProvider provider,
                     StateConsumer consumer);
  ~StateSync();

  StateSync(const StateSync &) = delete;
  StateSync &operator=(const StateSync &) = delete;

  // Start the TCP accept loop in a background thread.
  void startServer();

  // Stop the accept loop and join the server thread.
  void stopServer();

  bool serverRunning() const { return serverRunning_.load(); }

  // Try each peer in order until one successfully delivers the full state.
  // Each peer address is "ip:port"; if no port is given, tcpPort_ is used.
  // timeoutPerPeerMs: per-peer deadline covering connect + full transfer.
  // Returns true if state was loaded, false if every peer failed.
  bool requestState(const std::vector<std::string> &peers,
                    int timeoutPerPeerMs = 3000);

  // Static client-only overload: connect to each peer in turn without
  // constructing a StateSync server object. Useful when only the client
  // role is needed (e.g. Pipeline::syncState).
  static bool requestState(const std::vector<std::string> &peers,
                           uint16_t tcpPort, const StateConsumer &consumer,
                           int timeoutPerPeerMs = 3000);

private:
  void serverLoop();
  void handleClient(int clientFd);
  bool tryPeer(const std::string &addr, int timeoutMs);

  uint16_t tcpPort_;
  StateProvider provider_;
  StateConsumer consumer_;

  int serverFd_{-1};
  std::thread serverThread_;
  std::atomic<bool> serverRunning_{false};

  static constexpr uint8_t MSG_STATE_REQUEST = 0x01;
};
