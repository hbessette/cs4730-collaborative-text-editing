// p2p-editor: collaborative text editor with CRDT backend and FTXUI front end.
//
// Usage:
//   ./p2p-editor [--port DATA_PORT] [--peer IP:DATA_PORT ...] [--first]
//
//   --port P    UDP data port (default 5000). Heartbeat = P+1, TCP sync = P+2,
//               cursor sync = P+3.
//   --peer ADDR Known peer "ip:data-port". Can be repeated.
//   --first     Skip initial state-sync (this node starts the document).
//
// Keyboard:
//   Printable chars / Enter    insert at cursor
//   Backspace / Delete         delete character
//   Arrow keys / Home / End    move cursor
//   Escape or Ctrl+X           quit gracefully (broadcasts LEAVE)

#include "cursor_sync.h"
#include "editor_ui.h"
#include "logger.h"
#include "net_utils.h"
#include "peer_manager.h"
#include "peer_socket.h"
#include "pipeline.h"
#include "rga.h"
#include "state_sync.h"

#include "ftxui/component/event.hpp"
#include "ftxui/component/screen_interactive.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace ftxui;

struct Args {
  uint16_t dataPort = 5000;
  bool isFirst = false;
  std::vector<std::string> peerDataAddrs;
  std::string logPath;
};

static Args parseArgs(int argc, char *argv[]) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if ((arg == "--port" || arg == "-p") && i + 1 < argc)
      args.dataPort = static_cast<uint16_t>(std::stoi(argv[++i]));
    else if (arg == "--peer" && i + 1 < argc)
      args.peerDataAddrs.push_back(argv[++i]);
    else if (arg == "--first")
      args.isFirst = true;
    else if (arg == "--log-path" && i + 1 < argc)
      args.logPath = argv[++i];
  }
  return args;
}

// For each "ip:dataPort" peer address, registers:
//   - the data address with the UDP socket (CRDT ops)
//   - the heartbeat address (dataPort+1) with PeerManager
//   - the cursor address (dataPort+3) with CursorSync
//   - the TCP sync address (dataPort+2) in the returned list
static std::vector<std::string>
registerPeers(const std::vector<std::string> &peerDataAddrs,
              PeerSocket &dataSocket, PeerManager &peerMgr,
              CursorSync &cursorSync) {

  std::vector<std::string> syncAddrs;
  for (const auto &addr : peerDataAddrs) {
    dataSocket.addPeer(addr);

    std::string ip;
    uint16_t peerDataPort = 5000;
    parseAddr(addr, ip, peerDataPort, 5000);
    const auto ports = PortLayout::fromData(peerDataPort);
    peerMgr.addKnownPeer(ip + ":" + std::to_string(ports.hb));
    syncAddrs.push_back(ip + ":" + std::to_string(ports.tcp));
    cursorSync.addPeer(ip + ":" + std::to_string(ports.cursor));
  }
  return syncAddrs;
}

// Adjusts the shared cursor atomically using OT rules, broadcasts the updated
// position, then triggers a render.
static void setupRemoteOpCallback(Pipeline &pipeline, CursorSync &cursorSync,
                                  ScreenInteractive &screen,
                                  std::shared_ptr<std::atomic<int>> cursor) {
  pipeline.setOnRemoteOp([&screen, &cursorSync,
                          cursor](const Operation &op, int visOffset) {
    if (visOffset >= 0) {
      if (op.type == OpType::INSERT) {
        int expected = cursor->load();
        while (visOffset < expected)
          if (cursor->compare_exchange_weak(expected, expected + 1))
            break;
      } else {
        int expected = cursor->load();
        while (visOffset < expected)
          if (cursor->compare_exchange_weak(expected, expected - 1))
            break;
      }
      cursorSync.broadcast(cursor->load());
    }
    screen.PostEvent(Event::Custom);
  });
}

// Wires peer-join/leave callbacks.  Join registers the peer's data address
// with the UDP socket and their cursor address with CursorSync.
static void setupPeerCallbacks(PeerManager &peerMgr, PeerSocket &dataSocket,
                               Pipeline &pipeline, CursorSync &cursorSync,
                               ScreenInteractive &screen,
                               std::shared_ptr<NotifState> notif) {
  peerMgr.setOnPeerJoin([&dataSocket, &pipeline, &cursorSync, &screen](
                            uint32_t peerSiteID, const std::string &addr) {
    pipeline.addKnownSiteID(peerSiteID);
    std::string ip;
    uint16_t hbPort = 0;
    parseAddr(addr, ip, hbPort);
    if (!ip.empty() && hbPort != 0) {
      const auto ports =
          PortLayout::fromData(static_cast<uint16_t>(hbPort - 1));
      dataSocket.addPeer(ip + ":" + std::to_string(ports.data));
      cursorSync.addPeer(ip + ":" + std::to_string(ports.cursor));
    }
    screen.PostEvent(Event::Custom);
  });

  peerMgr.setOnPeerLeave([&pipeline, &cursorSync, &screen,
                          notif](uint32_t siteID, const std::string &) {
    pipeline.removeKnownSiteID(siteID);
    cursorSync.removePeer(siteID);

    // Show a transient disconnect message in the status bar.
    {
      std::lock_guard<std::mutex> lk(notif->mtx);
      notif->text = "Peer " + siteToHex(siteID) + " disconnected";
      notif->expires =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }

    screen.PostEvent(Event::Custom);
  });
}

int main(int argc, char *argv[]) {
  const Args args = parseArgs(argc, argv);

  const PortLayout myPorts = PortLayout::fromData(args.dataPort);

  const uint32_t siteID = generateSiteID();
  {
    const std::string hex = siteToHex(siteID);

    // Default log path: logs/<siteHex>.log (overridden by --log-path).
    std::string logPath = args.logPath;
    if (logPath.empty()) {
      mkdir("logs", 0755); // create directory if absent; ignore error if exists
      logPath = "logs/" + hex + ".log";
    }

#ifdef ENABLE_DEBUG_LOG
    Logger::init(logPath, siteID, LogLevel::DEBUG);
#else
    Logger::init(logPath, siteID, LogLevel::INFO);
#endif
    LOG_INFO("p2p-editor", "startup siteID=" + hex +
                               " port=" + std::to_string(args.dataPort) +
                               " log=" + logPath);
  }

  CRDTEngine crdt(siteID);
  PeerSocket dataSocket(myPorts.data);
  Pipeline pipeline(dataSocket, crdt);
  pipeline.addKnownSiteID(siteID); // own site is always known
  PeerManager peerMgr(myPorts.hb, siteID);
  CursorSync cursorSync(myPorts.cursor, siteID);

  auto syncAddrs =
      registerPeers(args.peerDataAddrs, dataSocket, peerMgr, cursorSync);

  StateSync stateServer(
      myPorts.tcp, [&pipeline] { return pipeline.serializeState(); },
      [](const std::vector<uint8_t> &) {});
  stateServer.startServer();

  auto screen = ScreenInteractive::Fullscreen();
  auto cursorShared = std::make_shared<std::atomic<int>>(0);
  std::atomic<bool> running{true};
  auto notif = std::make_shared<NotifState>();

  // Fire a re-render whenever a remote cursor update arrives.
  cursorSync.setOnUpdate([&screen] { screen.PostEvent(Event::Custom); });

  setupRemoteOpCallback(pipeline, cursorSync, screen, cursorShared);
  setupPeerCallbacks(peerMgr, dataSocket, pipeline, cursorSync, screen, notif);

  // When an op arrives from an unknown peer (late discovery / partition
  // recovery), request a full state sync from the current known peers.
  // activePeers() returns {siteID, "ip:hbPort"} pairs; TCP sync port =
  // hbPort+1. The callback spawns a detached thread to avoid blocking the CRDT
  // thread.
  pipeline.setOnUnknownPeer([&pipeline, &peerMgr, myPorts](uint32_t) {
    auto peerList = peerMgr.activePeers();
    if (peerList.empty())
      return;
    std::vector<std::string> tcpAddrs;
    for (const auto &p : peerList) {
      std::string ip;
      uint16_t hbPort = 0;
      parseAddr(p.second, ip, hbPort);
      if (!ip.empty() && hbPort != 0) {
        const auto ports =
            PortLayout::fromData(static_cast<uint16_t>(hbPort - 1));
        tcpAddrs.push_back(ip + ":" + std::to_string(ports.tcp));
      }
    }
    if (tcpAddrs.empty())
      return;
    std::thread([&pipeline, tcpAddrs, myPorts]() {
      pipeline.syncState(tcpAddrs, myPorts.tcp, /*timeoutPerPeerMs=*/3000);
    }).detach();
  });

  peerMgr.start();
  pipeline.start();
  cursorSync.start();

  if (!args.isFirst && !syncAddrs.empty())
    pipeline.syncState(syncAddrs, myPorts.tcp, /*timeoutPerPeerMs=*/3000);

  auto editor = MakeEditor(pipeline, peerMgr, cursorSync, screen, running,
                           cursorShared, notif);
  screen.Loop(editor);

  pipeline.stop();
  peerMgr.stop();
  cursorSync.stop();
  stateServer.stopServer();
  Logger::shutdown();

  return 0;
}
