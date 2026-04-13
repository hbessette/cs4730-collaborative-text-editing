#include "pipeline.h"
#include "serializer.h"

Pipeline::Pipeline(PeerSocket &socket, CRDTEngine &crdt)
    : socket_(socket), crdt_(crdt) {}

Pipeline::~Pipeline() {
  if (running_.load())
    stop();
}

void Pipeline::setOnRemoteOp(RemoteOpCallback cb) {
  onRemoteOp_ = std::move(cb);
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
  }
}

void Pipeline::receiveLoop() {
  while (running_.load()) {
    try {
      auto result = socket_.receive(100);
      Operation op = Serializer::decode(result.first);
      incoming_.push(op);
    } catch (const std::runtime_error &) {
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
      }
    }
    if (!buffered && onRemoteOp_)
      onRemoteOp_(op, visOffset);
  }
}

// Repeatedly scan for overlong lines and insert '\n' until the document is
// fully wrapped.  Re-fetching the doc after each fix is safe because
// localInsert acquires crdtMutex_ independently.
void Pipeline::reflowDocument() {
  bool fixed = true;
  while (fixed) {
    fixed = false;
    std::string doc = getDocument();
    int lineStart = 0;
    for (int i = 0; i <= static_cast<int>(doc.size()); ++i) {
      bool atEnd = (i == static_cast<int>(doc.size()));
      if (atEnd || doc[i] == '\n') {
        int lineLen = i - lineStart;
        if (lineLen > MAX_LINE_WIDTH) {
          localInsert(lineStart + MAX_LINE_WIDTH, '\n');
          fixed = true;
          break;
        }
        lineStart = i + 1;
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

  bool ok = false;
  StateSync syncer(
      tcpPort,
      /*provider=*/[] { return std::vector<uint8_t>{}; },
      /*consumer=*/
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
          for (const auto &[op, vis] : toNotify)
            onRemoteOp_(op, vis);
      });

  ok = syncer.requestState(peers, timeoutPerPeerMs);

  if (!ok) {
    std::lock_guard<std::mutex> lk(crdtMutex_);
    syncing_ = false;
    syncBuffer_.clear();
  }

  return ok;
}
