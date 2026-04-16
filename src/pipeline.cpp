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

static long long wallUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
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
    LOG_INFO("latency", std::string("LATENCY_SEND siteID=") +
                            std::to_string(op.id.siteID) + " clock=" +
                            std::to_string(op.id.clock) + " ts_us=" +
                            std::to_string(wallUs()));
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

// Apply one op to the CRDT (must hold crdtMutex_).  Emits LATENCY_APPLY and
// returns the visible offset; does NOT fire onRemoteOp_ or unknown-peer logic.
static void applyOneCrdt(CRDTEngine &crdt, const Operation &op) {
  crdt.applyRemote(op);
  LOG_INFO("latency", std::string("LATENCY_APPLY siteID=") +
                          std::to_string(op.id.siteID) + " clock=" +
                          std::to_string(op.id.clock) + " ts_us=" +
                          std::to_string(wallUs()));
  LOG_DEBUG("pipeline", std::string("OP_APPLY type=") +
                            opTypeName(op.type) +
                            " clock=" + std::to_string(op.id.clock) +
                            " siteID=" + std::to_string(op.id.siteID));
}

// Drain pendingOps_: repeatedly scan until no op can be applied.  Must hold
// crdtMutex_.  Returns a list of (op, visOffset) for caller to notify.
static std::vector<std::pair<Operation, int>>
drainPending(CRDTEngine &crdt, std::deque<Operation> &pending) {
  std::vector<std::pair<Operation, int>> notified;
  bool progress = true;
  while (progress && !pending.empty()) {
    progress = false;
    for (auto it = pending.begin(); it != pending.end();) {
      if (crdt.canApply(*it)) {
        applyOneCrdt(crdt, *it);
        notified.push_back({*it, crdt.visibleOffsetOf(it->id)});
        it = pending.erase(it);
        progress = true;
      } else {
        ++it;
      }
    }
  }
  return notified;
}

void Pipeline::crdtLoop() {
  Operation op;
  while (incoming_.pop(op, stopped_)) {
    std::vector<std::pair<Operation, int>> toNotify;
    bool buffered = false;

    {
      std::lock_guard<std::mutex> lk(crdtMutex_);
      if (syncing_) {
        syncBuffer_.push_back(op);
        buffered = true;
      } else if (!crdt_.canApply(op)) {
        // Dependency not yet in the CRDT (UDP out-of-order delivery).
        // Buffer and retry when the missing predecessor arrives.
        pendingOps_.push_back(op);
        buffered = true;
      } else {
        applyOneCrdt(crdt_, op);
        int visOffset = crdt_.visibleOffsetOf(op.id);
        toNotify.push_back({op, visOffset});
        // Each successful apply may unblock pending ops.
        auto drained = drainPending(crdt_, pendingOps_);
        toNotify.insert(toNotify.end(), drained.begin(), drained.end());
      }
    }

    for (const auto &entry : toNotify)
      if (onRemoteOp_)
        onRemoteOp_(entry.first, entry.second);

    if (!buffered) {
      // Detect ops from previously unseen peers and trigger a state sync.
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
          // Emit LATENCY_APPLY for every op included in the state snapshot so
          // the latency analysis script can match them against LATENCY_SEND
          // entries from the sender (joined on siteID + clock).
          for (const auto &id : crdt_.getNodeIDs()) {
            LOG_INFO("latency", std::string("LATENCY_APPLY siteID=") +
                                    std::to_string(id.siteID) + " clock=" +
                                    std::to_string(id.clock) + " ts_us=" +
                                    std::to_string(wallUs()));
          }
          // Apply buffered ops in arrival order; those that are still
          // out-of-order go into pendingOps_ and are drained at the end.
          for (const auto &bop : syncBuffer_) {
            if (crdt_.canApply(bop)) {
              applyOneCrdt(crdt_, bop);
              toNotify.push_back({bop, crdt_.visibleOffsetOf(bop.id)});
              auto drained = drainPending(crdt_, pendingOps_);
              toNotify.insert(toNotify.end(), drained.begin(), drained.end());
            } else {
              pendingOps_.push_back(bop);
            }
          }
          // Final drain pass in case any pending ops are now satisfiable.
          auto drained = drainPending(crdt_, pendingOps_);
          toNotify.insert(toNotify.end(), drained.begin(), drained.end());
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