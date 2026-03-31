#pragma once

#include <list>
#include <stdexcept>
#include <string>
#include <unordered_map>

struct CharID {
  int clock = 0;
  int siteID = 0;

  bool operator==(const CharID &o) const {
    return clock == o.clock && siteID == o.siteID;
  }
  bool operator!=(const CharID &o) const { return !(*this == o); }
  bool operator<(const CharID &o) const {
    return clock != o.clock ? clock < o.clock : siteID < o.siteID;
  }
  bool operator>(const CharID &o) const { return o < *this; }
};

static constexpr CharID SENTINEL_ID{0, 0};

struct CharIDHash {
  std::size_t operator()(const CharID &id) const noexcept {
    std::size_t h = std::hash<int>{}(id.clock);
    h ^= std::hash<int>{}(id.siteID) * 2654435761u;
    return h;
  }
};

enum class OpType { INSERT, DELETE };

struct Operation {
  OpType type;
  CharID id;
  CharID leftNeighborID;
  char value;
};

class CRDTEngine {
public:
  explicit CRDTEngine(int siteID);
  Operation localInsert(int pos, char c);
  Operation localDelete(int pos);
  void applyRemote(const Operation &op);
  std::string getDocument() const;

  int getSiteID() const { return siteID_; }
  int getLamportClock() const { return clock_; }

private:
  struct Node {
    CharID id;
    char value;
    bool tombstoned;
    CharID leftNeighborID;
  };

  using SeqIter = std::list<Node>::iterator;
  using IdMap = std::unordered_map<CharID, SeqIter, CharIDHash>;

  int siteID_;
  int clock_;
  std::list<Node> seq_;
  IdMap index_;

  CharID visibleAt(int n) const;
  SeqIter visibleIter(int n);
  CharID rootAncestor(CharID id, const CharID &targetParent);
  void insertNode(const CharID &newID, char value, const CharID &leftID);
};
