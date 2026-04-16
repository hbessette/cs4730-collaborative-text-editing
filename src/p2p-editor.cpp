// p2p-editor: collaborative text editor with CRDT backend and FTXUI front end.
//
// Usage:
//   ./p2p-editor [--port DATA_PORT] [--peer IP:DATA_PORT ...] [--first]
//               [--headless [--script FILE]]
//
//   --port P       UDP data port (default 10000). Heartbeat = P+1, TCP sync =
//   P+2,
//                  cursor sync = P+3.
//   --peer ADDR    Known peer "ip:data-port". Can be repeated.
//   --first        Skip initial state-sync (this node starts the document).
//   --headless     Run without a terminal UI (reads commands from stdin or
//   --script).
//   --script FILE  Script file for headless mode (default: stdin).
//   --log-path F   Override default log path (logs/<siteHex>.log).
//
// Keyboard (interactive mode):
//   Printable chars / Enter    insert at cursor
//   Backspace / Delete         delete character
//   Arrow keys / Home / End    move cursor
//   Escape or Ctrl+X           quit gracefully (broadcasts LEAVE)
//
// Headless script commands (one per line; '#' and blank lines are ignored):
//   INSERT <pos> <char>  – insert char at visible position pos
//   DELETE <pos>         – delete char at visible position pos
//   SLEEP <ms>           – sleep for ms milliseconds
//   DUMP                 – write current document text to stdout
//   QUIT                 – exit early (EOF also exits)

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
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace ftxui;

struct Args {
  uint16_t dataPort = 10000;
  bool isFirst = false;
  bool headless = false;
  std::string scriptPath; // empty = read from stdin (headless mode only)
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
    else if (arg == "--headless")
      args.headless = true;
    else if (arg == "--script" && i + 1 < argc)
      args.scriptPath = argv[++i];
    else if (arg == "--log-path" && i + 1 < argc)
      args.logPath = argv[++i];
  }
  return args;
}

static void initLogging(const Args &args, uint32_t siteID) {
  const std::string hex = siteToHex(siteID);
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
  LOG_INFO("p2p-editor", "startup siteID=" + hex + " port=" +
                             std::to_string(args.dataPort) + " log=" + logPath);
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
    uint16_t peerDataPort = 10000;
    parseAddr(addr, ip, peerDataPort, 10000);
    const auto ports = PortLayout::fromData(peerDataPort);
    peerMgr.addKnownPeer(ip + ":" + std::to_string(ports.hb));
    syncAddrs.push_back(ip + ":" + std::to_string(ports.tcp));
    cursorSync.addPeer(ip + ":" + std::to_string(ports.cursor));
  }
  return syncAddrs;
}

// When an op arrives from an unknown peer (late discovery / partition
// recovery), request a full state sync from the current known peers. Spawns a
// detached thread to avoid blocking the CRDT thread.
static void setupUnknownPeerCallback(Pipeline &pipeline, PeerManager &peerMgr,
                                     const PortLayout &myPorts) {
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
}

// Adjusts the shared cursor atomically using OT rules, broadcasts the updated
// position, then triggers a render.
static void setupRemoteOpCallback(Pipeline &pipeline, CursorSync &cursorSync,
                                  ScreenInteractive &screen,
                                  std::shared_ptr<std::atomic<int>> cursor) {
  pipeline.setOnRemoteOp(
      [&screen, &cursorSync, cursor](const Operation &op, int visOffset) {
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

// Wires peer-join/leave callbacks for interactive mode. Join registers the
// peer's data and cursor addresses; leave shows a transient status bar message.
static void setupInteractivePeerCallbacks(PeerManager &peerMgr,
                                          PeerSocket &dataSocket,
                                          Pipeline &pipeline,
                                          CursorSync &cursorSync,
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

// Wires peer-join/leave callbacks for headless mode (no screen redraws).
static void setupHeadlessPeerCallbacks(PeerManager &peerMgr,
                                       PeerSocket &dataSocket,
                                       Pipeline &pipeline,
                                       CursorSync &cursorSync) {
  peerMgr.setOnPeerJoin([&dataSocket, &pipeline, &cursorSync](
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
  });

  peerMgr.setOnPeerLeave(
      [&pipeline, &cursorSync](uint32_t siteID, const std::string &) {
        pipeline.removeKnownSiteID(siteID);
        cursorSync.removePeer(siteID);
      });
}

// Headless script interpreter. Reads commands from scriptPath (or stdin if
// scriptPath is empty) and applies them directly to the pipeline.
static void runHeadless(Pipeline &pipeline, const std::string &scriptPath) {
  std::ifstream fileStream;
  std::istream *in = &std::cin;
  if (!scriptPath.empty()) {
    fileStream.open(scriptPath);
    if (!fileStream) {
      std::cerr << "Cannot open script: " << scriptPath << "\n";
      return;
    }
    in = &fileStream;
  }

  std::string line;
  while (std::getline(*in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;

    if (cmd == "INSERT") {
      int pos;
      char c;
      ss >> pos >> c;
      pipeline.localInsert(pos, c);
    } else if (cmd == "DELETE") {
      int pos;
      ss >> pos;
      pipeline.localDelete(pos);
    } else if (cmd == "SLEEP") {
      int ms;
      ss >> ms;
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    } else if (cmd == "DUMP") {
      std::cout << pipeline.getDocument() << std::flush;
    } else if (cmd == "QUIT") {
      break;
    }
  }
}

// Creates the fullscreen FTXUI terminal UI, wires all screen-dependent
// callbacks, and runs the event loop until the user quits.
static void runInteractive(Pipeline &pipeline, PeerManager &peerMgr,
                           PeerSocket &dataSocket, CursorSync &cursorSync) {
  auto screen = ScreenInteractive::Fullscreen();
  auto cursorShared = std::make_shared<std::atomic<int>>(0);
  std::atomic<bool> running{true};
  auto notif = std::make_shared<NotifState>();

  cursorSync.setOnUpdate([&screen] { screen.PostEvent(Event::Custom); });
  setupRemoteOpCallback(pipeline, cursorSync, screen, cursorShared);
  setupInteractivePeerCallbacks(peerMgr, dataSocket, pipeline, cursorSync,
                                screen, notif);

  auto editor = MakeEditor(pipeline, peerMgr, cursorSync, screen, running,
                           cursorShared, notif);
  screen.Loop(editor);
}

int main(int argc, char *argv[]) {
  const Args args = parseArgs(argc, argv);
  const PortLayout myPorts = PortLayout::fromData(args.dataPort);
  const uint32_t siteID = generateSiteID();

  initLogging(args, siteID);

  CRDTEngine crdt(siteID);
  PeerSocket dataSocket(myPorts.data);
  Pipeline pipeline(dataSocket, crdt);
  pipeline.addKnownSiteID(siteID); // own site is always known
  PeerManager peerMgr(myPorts.hb, siteID);
  CursorSync cursorSync(myPorts.cursor, siteID);

  const auto syncAddrs =
      registerPeers(args.peerDataAddrs, dataSocket, peerMgr, cursorSync);

  StateSync stateServer(
      myPorts.tcp, [&pipeline] { return pipeline.serializeState(); },
      [](const std::vector<uint8_t> &) {});

  stateServer.startServer();

  // In headless mode all peers are pre-registered via --peer, and the initial
  // syncState handles bootstrap.  Re-syncing on unknown-peer detection would
  // call loadState (full state replacement), clobbering local ops that the
  // remote peer hasn't yet received — causing CRDT divergence under high
  // concurrency.  Out-of-order UDP delivery is handled by pendingOps_ instead.
  if (!args.headless)
    setupUnknownPeerCallback(pipeline, peerMgr, myPorts);

  if (args.headless)
    setupHeadlessPeerCallbacks(peerMgr, dataSocket, pipeline, cursorSync);

  peerMgr.start();
  cursorSync.start();

  // Sync initial state before starting the UDP receive/CRDT threads.
  // syncState uses TCP only; starting pipeline first creates a race where
  // crdtLoop can apply UDP ops to the empty CRDT before syncing_ is set.
  if (!args.isFirst && !syncAddrs.empty())
    pipeline.syncState(syncAddrs, myPorts.tcp, /*timeoutPerPeerMs=*/3000);

  pipeline.start();

  if (args.headless)
    runHeadless(pipeline, args.scriptPath);
  else
    runInteractive(pipeline, peerMgr, dataSocket, cursorSync);

  pipeline.stop();
  peerMgr.stop();
  cursorSync.stop();
  stateServer.stopServer();
  Logger::shutdown();

  return 0;
}
