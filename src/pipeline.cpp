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
    {
      std::lock_guard<std::mutex> lk(crdtMutex_);
      crdt_.applyRemote(op);
    }
    if (onRemoteOp_)
      onRemoteOp_(op);
  }
}
