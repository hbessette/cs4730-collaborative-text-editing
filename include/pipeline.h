#pragma once

#include "peer_socket.h"
#include "rga.h"
#include "serializer.h"
#include "state_sync.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Thread-safe MPSC (multi-producer, single-consumer) queue.
//
// push() is safe to call from any number of threads concurrently.
// pop() is meant for a single consumer thread; it blocks until an item is
// available or the *stop flag becomes true, at which point it returns false
// immediately (even if items remain in the queue).
template <typename T> class MpscQueue {
public:
  // Push an item. Safe to call from any thread.
  void push(T item) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      q_.push_back(std::move(item));
    }
    cv_.notify_one();
  }

  // Block until an item is available or stop.load() is true.
  // Returns true and moves the front item into out on success.
  // Returns false (without modifying out) when stop is set.
  bool pop(T &out, const std::atomic<bool> &stop) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&] { return !q_.empty() || stop.load(); });
    if (stop.load())
      return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  // Wake any blocked pop() call (used during shutdown).
  void wake() { cv_.notify_all(); }

private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<T> q_;
};

// Pipeline: three-thread send/receive/apply pipeline for collaborative editing.
// CRDTEngine is protected by crdtMutex_. All access to crdt_ — whether from
// the UI thread (localInsert / localDelete / getDocument) or from the CRDT
// thread (applyRemote) — must hold crdtMutex_.  This guarantees no data races
// on the engine and is verifiable with ThreadSanitizer.
class Pipeline {
public:
  // Maximum visible line length shared by all peers (matches editor_ui.cpp).
  // Any line that grows past this limit is automatically wrapped by inserting
  // '\n' at this column, keeping all peers' documents in sync.
  static constexpr int MAX_LINE_WIDTH = 120;

  // Callback invoked by the CRDT thread after each remote operation is applied.
  // visibleOffset is the visible-string position of the affected character,
  // computed under crdtMutex_ before the lock is released — so it is
  // guaranteed to be consistent with the document state the callback sees.
  // For INSERT: position of the newly inserted char.
  // For DELETE: former visible position of the deleted char (-1 if not found).
  using RemoteOpCallback =
      std::function<void(const Operation &, int visibleOffset)>;

  // socket: UDP socket used for sending and receiving operation messages.
  // crdt:   CRDT engine; Pipeline becomes the sole path for all crdt_ access.
  explicit Pipeline(PeerSocket &socket, CRDTEngine &crdt);
  ~Pipeline();

  Pipeline(const Pipeline &) = delete;
  Pipeline &operator=(const Pipeline &) = delete;

  // Register a callback invoked by the CRDT thread after a remote op is
  // applied. Must be called before start().
  void setOnRemoteOp(RemoteOpCallback cb);

  // Spawn the three background threads (send, receive, CRDT).
  void start();

  // Signal all threads to stop and join them.
  void stop();

  bool isRunning() const { return running_.load(); }

  // Apply a local insert to the CRDT and enqueue the resulting op for
  // broadcast.
  Operation localInsert(int pos, char c);

  // Apply a local delete to the CRDT and enqueue the resulting op for
  // broadcast.
  Operation localDelete(int pos);

  // Return a snapshot of the current document text.
  std::string getDocument();

  // Return the visible offset (0-based) of the character with the given ID,
  // counting only non-tombstoned characters that precede it.  Returns -1 if
  // the ID is not found or is SENTINEL_ID.  Thread-safe (holds crdtMutex_).
  int visibleOffsetOf(const CharID &id);

  // Enqueue an already-constructed Operation for broadcast without touching
  // crdt_. Useful for replaying or forwarding operations.
  void sendOperation(const Operation &op);

  // Scan the document for lines longer than MAX_LINE_WIDTH and insert '\n' at
  // column MAX_LINE_WIDTH until no such lines remain.  Intended to be called
  // from the remote-op callback so that inserts from other peers that push a
  // line over the limit are automatically reflowed on this peer too.
  void reflowDocument();

  // Return the full serialized CRDT state (thread-safe).
  // Suitable as the StateSync::StateProvider callback.
  std::vector<uint8_t> serializeState();

  // Request full document state from one of the given TCP peers ("ip:port").
  // While the transfer is in progress, incoming remote ops are buffered and
  // applied (idempotently) after the state is loaded.
  // Returns true on success, false if every peer failed or timed out.
  bool syncState(const std::vector<std::string> &peers, uint16_t tcpPort,
                 int timeoutPerPeerMs = 3000);

private:
  void sendLoop();
  void receiveLoop();
  void crdtLoop();

  PeerSocket &socket_;
  CRDTEngine &crdt_;
  std::mutex crdtMutex_; // guards all access to crdt_

  MpscQueue<Operation> outgoing_; // UI thread   → send thread
  MpscQueue<Operation> incoming_; // recv thread → CRDT thread

  std::thread sendThread_;
  std::thread receiveThread_;
  std::thread crdtThread_;

  std::atomic<bool> running_{false};
  std::atomic<bool> stopped_{false}; // true = signal pop() callers to exit

  // Sync-buffer: while syncing_ is true (under crdtMutex_), the CRDT thread
  // appends incoming ops here instead of applying them immediately.
  bool syncing_{false};               // guarded by crdtMutex_
  std::vector<Operation> syncBuffer_; // guarded by crdtMutex_

  RemoteOpCallback onRemoteOp_; // set before start(), read-only afterwards
};
