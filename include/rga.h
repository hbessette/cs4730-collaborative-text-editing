#pragma once

#include <cstdint>
#include <list>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Logical timestamp that uniquely identifies a single character in the
// Replicated Growable Array (RGA).
//
// Total order: (clock, siteID) — ascending clock first, then ascending siteID
// to break ties among characters inserted at the same logical time by different
// peers.  This order is used by insertNode() to deterministically resolve
// concurrent inserts at the same position.
struct CharID {
  int clock;
  uint32_t siteID;

  constexpr CharID(int clock = 0, uint32_t siteID = 0)
      : clock(clock), siteID(siteID) {}

  bool operator==(const CharID &o) const {
    return clock == o.clock && siteID == o.siteID;
  }
  bool operator!=(const CharID &o) const { return !(*this == o); }
  // Ascending order: lower clock first; equal clock breaks tie by siteID.
  bool operator<(const CharID &o) const {
    return clock != o.clock ? clock < o.clock : siteID < o.siteID;
  }
  bool operator>(const CharID &o) const { return o < *this; }
};

// Permanent head of every RGA sequence.  Every insert's leftNeighborID chain
// terminates at this sentinel, and it is never tombstoned or removed.
static constexpr CharID SENTINEL_ID{0, 0};

// FNV-inspired hash for CharID — used by the O(1) index_ lookup map.
struct CharIDHash {
  std::size_t operator()(const CharID &id) const noexcept {
    std::size_t h = std::hash<int>{}(id.clock);
    h ^= std::hash<int>{}(id.siteID) * 2654435761u;
    return h;
  }
};

// Distinguishes the two kinds of operations that can be applied to the CRDT.
enum class OpType { INSERT, DELETE };

// The unit of replication.  Produced by localInsert/localDelete and consumed
// by applyRemote on remote peers.
//
//   INSERT: insert character `value` with identity `id` immediately after the
//           node identified by `leftNeighborID`.
//   DELETE: tombstone the node identified by `id`; leftNeighborID and value
//           are unused for deletes and are set to SENTINEL_ID / '\0'.
struct Operation {
  OpType type;
  CharID id;
  CharID leftNeighborID;
  char value;
};

// RGA (Replicated Growable Array) CRDT engine.
//
// Maintains the document as a linked list of Node records (seq_) with an
// accompanying hash-map index (index_) for O(1) lookup by CharID.  Deleted
// characters are tombstoned rather than removed so that concurrent operations
// referencing their IDs remain valid.
//
// Thread safety: NOT thread-safe on its own.  All external access must be
// serialised by the caller (Pipeline uses crdtMutex_ for this purpose).
class CRDTEngine {
public:
  // Construct an empty document owned by `siteID`.  Initialises the sentinel
  // node and sets the Lamport clock to 0.
  explicit CRDTEngine(uint32_t siteID);

  // Insert character `c` at visible position `pos` (0-based, counting only
  // non-tombstoned characters).  Increments the Lamport clock, assigns a
  // unique CharID, and returns the Operation to broadcast to peers.
  // Throws std::out_of_range if pos > visible document length.
  Operation localInsert(int pos, char c);

  // Delete the character at visible position `pos`.  Marks the node as
  // tombstoned and increments the Lamport clock.  Returns the DELETE Operation
  // to broadcast.  Throws std::out_of_range if pos is out of range.
  Operation localDelete(int pos);

  // Integrate a remote operation produced by another peer.
  // Updates the Lamport clock to max(clock_, op.id.clock + 1).
  // INSERT: idempotent — duplicate inserts (same CharID already in index_)
  //         are silently ignored.
  // DELETE: idempotent — tombstoning an already-tombstoned or absent node
  //         is a no-op.
  // Precondition for INSERT: op.leftNeighborID must already be in index_.
  // Use canApply() to check before calling; Pipeline::pendingOps_ handles
  // out-of-order delivery.
  void applyRemote(const Operation &op);

  // Return the current document text (all non-tombstoned characters in order).
  std::string getDocument() const;

  // Return this peer's unique site identifier.
  uint32_t getSiteID() const { return siteID_; }

  // Return the current Lamport clock value.
  int getLamportClock() const { return clock_; }

  // Replace this engine's document state with other's, keeping our siteID.
  // Rebuilds the internal index after move. O(n) in document length.
  void loadState(CRDTEngine &&other);

  // Return the number of non-tombstoned characters that precede the node with
  // the given id in the sequence.  Works for both visible and tombstoned nodes:
  // for a tombstoned node the return value equals the visible position the
  // character held just before it was deleted.  Returns -1 if id is not found
  // or if id == SENTINEL_ID.
  int visibleOffsetOf(const CharID &id) const;

  // Return all CharIDs in the sequence (excluding the sentinel).
  // Used to emit LATENCY_APPLY log entries after a state snapshot is loaded.
  std::vector<CharID> getNodeIDs() const;

  // Returns true if op can be applied without a missing-dependency crash.
  // An INSERT requires its leftNeighborID to already be in the index.
  // A DELETE always returns true (no-ops gracefully if the target is absent).
  bool canApply(const Operation &op) const;

private:
  struct Node {
    CharID id;
    char value;
    bool tombstoned;
    CharID leftNeighborID;
  };

  using SeqIter = std::list<Node>::iterator;
  using IdMap = std::unordered_map<CharID, SeqIter, CharIDHash>;

  uint32_t siteID_;
  int clock_;
  std::list<Node> seq_;
  IdMap index_;

  CharID visibleAt(int n) const;
  SeqIter visibleIter(int n);
  CharID rootAncestor(CharID id, const CharID &targetParent);
  void insertNode(const CharID &newID, char value, const CharID &leftID);

  friend class Serializer;
};
