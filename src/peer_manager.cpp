#include "peer_manager.h"
#include "logger.h"
#include "net_utils.h"
#include "serializer.h"

#include <arpa/inet.h>

#include <cstring>
#include <random>
#include <stdexcept>

uint32_t generateSiteID() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint32_t> dist(1, 0xFFFFFFFFu);
  return dist(gen);
}

PeerManager::PeerManager(uint16_t heartbeatPort, uint32_t siteID)
    : socket_(heartbeatPort), siteID_(siteID) {}

PeerManager::~PeerManager() {
  if (running_.load())
    stop();
}

void PeerManager::setOnPeerJoin(PeerCallback cb) { joinCb_ = std::move(cb); }
void PeerManager::setOnPeerLeave(PeerCallback cb) { leaveCb_ = std::move(cb); }

void PeerManager::addKnownPeer(const std::string &addr) {
  socket_.addPeer(addr);
}

void PeerManager::start() {
  if (running_.exchange(true))
    return;
  sendMsg(HBMsgType::JOIN);
  thread_ = std::thread(&PeerManager::run, this);
}

void PeerManager::stop() {
  if (!running_.exchange(false))
    return;
  sendMsg(HBMsgType::LEAVE);
  if (thread_.joinable())
    thread_.join();
}

std::vector<std::pair<uint32_t, std::string>> PeerManager::activePeers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<uint32_t, std::string>> out;
  for (const auto &kv : peers_)
    out.push_back({kv.first, kv.second.addr});
  return out;
}

void PeerManager::run() {
  using clock = std::chrono::steady_clock;
  auto lastHeartbeat = clock::now();

  while (running_.load()) {
    try {
      auto result = socket_.receive(200);
      processMsg(result.first, result.second);
    } catch (const std::runtime_error &e) {
      if (std::strstr(e.what(), "timeout") == nullptr)
        break;
    }

    auto now = clock::now();

    auto msSinceHB = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - lastHeartbeat)
                         .count();
    if (msSinceHB >= HEARTBEAT_INTERVAL_MS) {
      sendMsg(HBMsgType::HEARTBEAT);
      lastHeartbeat = now;
    }

    checkDeadPeers();
  }
}

void PeerManager::sendMsg(HBMsgType type) {
  struct sockaddr_in sa;
  socklen_t len = sizeof(sa);
  getsockname(socket_.fd(), reinterpret_cast<struct sockaddr *>(&sa), &len);
  uint16_t listenPort = ntohs(sa.sin_port);

  std::vector<uint8_t> msg;
  msg.reserve(7);
  msg.push_back(static_cast<uint8_t>(type));
  Serializer::writeUint32(msg, siteID_);
  Serializer::writeUint16(msg, listenPort);
  socket_.send(msg);
}

void PeerManager::processMsg(const std::vector<uint8_t> &data,
                             const std::string &senderAddr) {
  if (data.size() < 7)
    return;

  HBMsgType type = static_cast<HBMsgType>(data[0]);
  size_t off = 1;
  uint32_t sender = Serializer::readUint32(data.data(), off);
  uint16_t port = Serializer::readUint16(data.data(), off);

  if (sender == siteID_)
    return;

  std::string ip;
  uint16_t senderPort = 0;
  parseAddr(senderAddr, ip, senderPort);
  std::string canonAddr = ip + ":" + std::to_string(port);

  if (type == HBMsgType::LEAVE) {
    std::string addr;
    PeerCallback cb;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = peers_.find(sender);
      if (it == peers_.end())
        return;
      addr = it->second.addr;
      peers_.erase(it);
      cb = leaveCb_;
    }
    LOG_INFO("peer_manager", "peer leave (graceful) siteID=" +
                                 siteToHex(sender) + " addr=" + addr);
    if (cb)
      cb(sender, addr);
    return;
  }

  bool isNew = false;
  PeerCallback joinCb;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(sender);
    if (it == peers_.end()) {
      peers_[sender] = {canonAddr, std::chrono::steady_clock::now()};
      isNew = true;
      joinCb = joinCb_;
    } else {
      it->second.lastSeen = std::chrono::steady_clock::now();
    }
  }

  if (isNew) {
    socket_.addPeer(canonAddr);
    sendMsg(HBMsgType::JOIN);
    LOG_INFO("peer_manager",
             "peer joined siteID=" + siteToHex(sender) + " addr=" + canonAddr);
    if (joinCb)
      joinCb(sender, canonAddr);
  }
}

void PeerManager::checkDeadPeers() {
  using clock = std::chrono::steady_clock;
  auto now = clock::now();

  std::vector<std::pair<uint32_t, std::string>> dead;
  PeerCallback cb;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = peers_.begin(); it != peers_.end();) {
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - it->second.lastSeen)
                    .count();
      if (ms > DEAD_THRESHOLD_MS) {
        dead.push_back({it->first, it->second.addr});
        it = peers_.erase(it);
      } else {
        ++it;
      }
    }
    cb = leaveCb_;
  }
  for (const auto &kv : dead) {
    LOG_WARN("peer_manager", "peer timeout siteID=" + siteToHex(kv.first) +
                                 " addr=" + kv.second);
    if (cb)
      cb(kv.first, kv.second);
  }
}
