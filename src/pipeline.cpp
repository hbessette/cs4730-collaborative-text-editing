#include "pipeline.h"
#include "logger.h"
#include "serializer.h"
#include "state_sync.h"

#include <chrono>
#include <cstring>

static long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static const char *opTypeName(OpType t) {
  return t == OpType::INSERT ? "INSERT" : "DELETE";
}

Pipeline::Pipeline(PeerSocket &socket, CRDTEngine &crdt)
    : socket_(socket), crdt_(crdt) {}

Pipeline::~Pipeline() {
  if (running_.load())
    stop();
}

void Pipeline::setOnRemoteOp(RemoteOpCallback cb) {
  onRemoteOp_ = std::move(cb);
}

void Pipeline::setOnUnknownPeer(std::function<void(uint32_t)> cb) {
  onUnknownPeer_ = std::move(cb);
}

void Pipeline::addKnownSiteID(uint32_t id) {
  std::lock_guard<std::mutex> lk(knownMutex_);
  knownSiteIDs_.insert(id);
}

void Pipeline::removeKnownSiteID(uint32_t id) {
  std::lock_guard<std::mutex> lk(knownMutex_);
  knownSiteIDs_.erase(id);
}

void Pipeline::start() {
  if (running_.exchange(true))
    return;
  stopped_.store(false);
  sendThread_ = std::thread(&Pipeline::sendLoop, this);
  receiveThread_ = std::thread(&Pipeline::receiveLoop, this);
  crdtThread_ = std::thread(&Pipeline::crdtLoop, this);
}

void Pipeline::stop() {
  if (!running_.exchange(false))
    return;
  stopped_.store(true);
  outgoing_.wake();
  incoming_.wake();
  if (sendThread_.joinable())
    sendThread_.join();
  if (receiveThread_.joinable())
    receiveThread_.join();
  if (crdtThread_.joinable())
    crdtThread_.join();
}

Operation Pipeline::localInsert(int pos, char c) {
  std::lock_guard<std::mutex> lk(crdtMutex_);
  Operation op = crdt_.localInsert(pos, c);
  outgoing_.push(op);
  return op;
}

Operation Pipeline::localDelete(int pos) {
  std::lock_guard<std::mutex> lk(crdtMutex_);
  Operation op = crdt_.localDelete(pos);
  outgoing_.push(op);
  return op;
}

std::string Pipeline::getDocument() {
  std::lock_guard<std::mutex> lk(crdtMutex_);
  return crdt_.getDocument();
}

int Pipeline::visibleOffsetOf(const CharID &id) {
  std::lock_guard<std::mutex> lk(crdtMutex_);
  return crdt_.visibleOffsetOf(id);
}

void Pipeline::sendOperation(const Operation &op) { outgoing_.push(op); }

void Pipeline::sendLoop() {
  Operation op;
  while (outgoing_.pop(op, stopped_)) {
    std::vector<uint8_t> bytes = Serializer::encode(op);
    socket_.send(bytes);
    LOG_DEBUG("pipeline", std::string("OP_SEND type=") + opTypeName(op.type) +
                              " clock=" + std::to_string(op.id.clock) +
                              " siteID=" + std::to_string(op.id.siteID) +
                              " ts_ms=" + std::to_string(nowMs()));
  }
}

void Pipeline::receiveLoop() {
  while (running_.load()) {
    try {
      auto result = socket_.receive(100);
      Operation op = Serializer::decode(result.first);
      incoming_.push(op);
      LOG_DEBUG("pipeline", std::string("OP_RECV type=") + opTypeName(op.type) +
                                " clock=" + std::to_string(op.id.clock) +
                                " siteID=" + std::to_string(op.id.siteID) +
                                " ts_ms=" + std::to_string(nowMs()));
    } catch (const std::runtime_error &e) {
      if (std::strstr(e.what(), "timeout") == nullptr)
        LOG_ERROR("pipeline", std::string("receive error: ") + e.what());
    }
  }
}

void Pipeline::crdtLoop() {
  Operation op;
  while (incoming_.pop(op, stopped_)) {
    bool buffered = false;
    int visOffset = -1;
    {
      std::lock_guard<std::mutex> lk(crdtMutex_);
      if (syncing_) {
        syncBuffer_.push_back(op);
        buffered = true;
      } else {
        crdt_.applyRemote(op);
        visOffset = crdt_.visibleOffsetOf(op.id);
        LOG_DEBUG("pipeline", std::string("OP_APPLY type=") +
                                  opTypeName(op.type) +
                                  " clock=" + std::to_string(op.id.clock) +
                                  " siteID=" + std::to_string(op.id.siteID));
      }
    }
    if (!buffered && onRemoteOp_)
      onRemoteOp_(op, visOffset);

    // Detect ops from previously unseen peers and trigger a state sync.
    // We add the siteID to knownSiteIDs_ immediately to suppress repeat
    // triggers for the same unknown peer.
    uint32_t sid = static_cast<uint32_t>(op.id.siteID);
    bool unknown = false;
    {
      std::lock_guard<std::mutex> lk(knownMutex_);
      if (knownSiteIDs_.find(sid) == knownSiteIDs_.end()) {
        knownSiteIDs_.insert(sid);
        unknown = true;
      }
    }
    if (unknown) {
      LOG_INFO("pipeline", "unknown peer siteID=" + std::to_string(sid) +
                               " triggering state sync");
      if (onUnknownPeer_)
        onUnknownPeer_(sid);
    }
  }
}


std::vector<uint8_t> Pipeline::serializeState() {
  std::lock_guard<std::mutex> lk(crdtMutex_);
  return Serializer::encodeState(crdt_);
}

bool Pipeline::syncState(const std::vector<std::string> &peers,
                         uint16_t tcpPort, int timeoutPerPeerMs) {
  {
    std::lock_guard<std::mutex> lk(crdtMutex_);
    syncing_ = true;
    syncBuffer_.clear();
  }
  LOG_INFO("pipeline", "syncState start peers=" + std::to_string(peers.size()));

  bool ok = StateSync::requestState(
      peers, tcpPort,
      [this](const std::vector<uint8_t> &bytes) {
        CRDTEngine newState = Serializer::decodeState(bytes);
        std::vector<std::pair<Operation, int>> toNotify;
        {
          std::lock_guard<std::mutex> lk(crdtMutex_);
          crdt_.loadState(std::move(newState));
          for (const auto &op : syncBuffer_) {
            crdt_.applyRemote(op);
            toNotify.push_back({op, crdt_.visibleOffsetOf(op.id)});
          }
          syncBuffer_.clear();
          syncing_ = false;
        }
        if (onRemoteOp_)
          for (const auto &entry : toNotify)
            onRemoteOp_(entry.first, entry.second);
      },
      timeoutPerPeerMs);

  if (ok) {
    LOG_INFO("pipeline", "syncState success");
  } else {
    LOG_WARN("pipeline", "syncState failed — all peers exhausted or timed out");
    std::lock_guard<std::mutex> lk(crdtMutex_);
    syncing_ = false;
    syncBuffer_.clear();
  }

  return ok;
}
