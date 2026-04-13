#include "cursor_sync.h"
#include "serializer.h"

#include <stdexcept>

CursorSync::CursorSync(uint16_t port, uint32_t siteID)
    : socket_(port), siteID_(siteID) {}

CursorSync::~CursorSync() {
  if (running_.load())
    stop();
}

void CursorSync::addPeer(const std::string &addr) { socket_.addPeer(addr); }

void CursorSync::removePeer(uint32_t siteID) {
  std::lock_guard<std::mutex> lk(mu_);
  remoteCursors_.erase(siteID);
}

void CursorSync::setOnUpdate(std::function<void()> cb) {
  onUpdate_ = std::move(cb);
}

void CursorSync::start() {
  if (running_.exchange(true))
    return;
  recvThread_ = std::thread(&CursorSync::recvLoop, this);
}

void CursorSync::stop() {
  if (!running_.exchange(false))
    return;
  if (recvThread_.joinable())
    recvThread_.join();
}

void CursorSync::broadcast(int pos) {
  if (!running_.load())
    return;
  socket_.send(Serializer::encodeCursor(siteID_, static_cast<int32_t>(pos)));
}

std::unordered_map<uint32_t, int> CursorSync::getRemoteCursors() const {
  std::lock_guard<std::mutex> lk(mu_);
  return remoteCursors_;
}

void CursorSync::recvLoop() {
  while (running_.load()) {
    try {
      auto [data, addr] = socket_.receive(100);
      uint32_t sid;
      int32_t pos;
      Serializer::decodeCursor(data, sid, pos);
      if (sid == siteID_)
        continue; // ignore our own reflected packets
      std::function<void()> cb;
      {
        std::lock_guard<std::mutex> lk(mu_);
        remoteCursors_[sid] = static_cast<int>(pos);
        cb = onUpdate_;
      }
      if (cb)
        cb();
    } catch (const std::runtime_error &) {
      // timeout or malformed packet — keep looping
    }
  }
}
