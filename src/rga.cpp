#include "rga.h"

void CRDTEngine::loadState(CRDTEngine &&other) {
  clock_ = other.clock_;
  seq_ = std::move(other.seq_);
  index_.clear();
  for (auto it = seq_.begin(); it != seq_.end(); ++it)
    index_[it->id] = it;
}

CRDTEngine::CRDTEngine(int siteID) : siteID_(siteID), clock_(0) {
  seq_.push_back({SENTINEL_ID, '\0', true, SENTINEL_ID});
  index_[SENTINEL_ID] = seq_.begin();
}

Operation CRDTEngine::localInsert(int pos, char c) {
  CharID leftID = visibleAt(pos - 1);
  CharID newID = {++clock_, siteID_};
  insertNode(newID, c, leftID);
  return {OpType::INSERT, newID, leftID, c};
}

Operation CRDTEngine::localDelete(int pos) {
  ++clock_;
  auto it = visibleIter(pos);
  if (it == seq_.end())
    throw std::out_of_range("localDelete: position out of range");
  it->tombstoned = true;
  return {OpType::DELETE, it->id, SENTINEL_ID, '\0'};
}

void CRDTEngine::applyRemote(const Operation &op) {
  if (op.id.clock >= clock_)
    clock_ = op.id.clock + 1;

  if (op.type == OpType::INSERT) {
    if (index_.count(op.id))
      return;
    insertNode(op.id, op.value, op.leftNeighborID);
  } else {
    auto it = index_.find(op.id);
    if (it != index_.end())
      it->second->tombstoned = true;
  }
}

std::string CRDTEngine::getDocument() const {
  std::string out;
  for (const auto &n : seq_)
    if (!n.tombstoned)
      out += n.value;
  return out;
}

CharID CRDTEngine::visibleAt(int n) const {
  if (n < 0)
    return SENTINEL_ID;
  int cnt = 0;
  for (const auto &node : seq_) {
    if (!node.tombstoned) {
      if (cnt == n)
        return node.id;
      ++cnt;
    }
  }
  return SENTINEL_ID;
}

CRDTEngine::SeqIter CRDTEngine::visibleIter(int n) {
  int cnt = 0;
  for (auto it = seq_.begin(); it != seq_.end(); ++it) {
    if (!it->tombstoned) {
      if (cnt == n)
        return it;
      ++cnt;
    }
  }
  return seq_.end();
}

CharID CRDTEngine::rootAncestor(CharID id, const CharID &targetParent) {
  while (true) {
    auto it = index_.find(id);
    if (it == index_.end())
      return SENTINEL_ID;
    const CharID &parent = it->second->leftNeighborID;
    if (parent == targetParent)
      return id;
    if (parent == SENTINEL_ID)
      return SENTINEL_ID;
    id = parent;
  }
}

int CRDTEngine::visibleOffsetOf(const CharID &id) const {
  if (id == SENTINEL_ID)
    return -1;
  int cnt = 0;
  for (const auto &node : seq_) {
    if (node.id == id)
      return cnt;
    if (!node.tombstoned)
      ++cnt;
  }
  return -1;
}

void CRDTEngine::insertNode(const CharID &newID, char value,
                            const CharID &leftID) {
  auto insertPos = std::next(index_.at(leftID));

  while (insertPos != seq_.end()) {
    CharID root = rootAncestor(insertPos->id, leftID);
    if (root != SENTINEL_ID && root > newID) {
      ++insertPos;
    } else {
      break;
    }
  }

  auto newIt = seq_.insert(insertPos, {newID, value, false, leftID});
  index_[newID] = newIt;
}
