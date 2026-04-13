#include "pipeline.h"
#include "rga.h"
#include "serializer.h"
#include "state_sync.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// UDP data ports
static const uint16_t SS_UDP_A = 6100;
static const uint16_t SS_UDP_B = 6101;

// TCP state-sync ports
static const uint16_t SS_TCP_A = 6200;
static const uint16_t SS_TCP_B = 6201;
static const uint16_t SS_TCP_CRASH = 6202; // for crash-simulation server

void test_load_state_replaces_document() {
  CRDTEngine src(1);
  src.localInsert(0, 'h');
  src.localInsert(1, 'i');

  CRDTEngine dst(2); // different site, empty
  dst.loadState(std::move(src));

  assert(dst.getDocument() == "hi");
  assert(dst.getSiteID() == 2 && "siteID must be preserved after loadState");
}

void test_load_state_clock_advances() {
  CRDTEngine src(1);
  src.localInsert(0, 'x'); // clock → 1
  int srcClock = src.getLamportClock();

  CRDTEngine dst(2);
  dst.loadState(std::move(src));

  assert(dst.getLamportClock() == srcClock);
}

void test_state_sync_server_client() {
  CRDTEngine serverCrdt(1);
  serverCrdt.localInsert(0, 'h');
  serverCrdt.localInsert(1, 'e');
  serverCrdt.localInsert(2, 'y');

  std::mutex serverMu;

  StateSync server(
      SS_TCP_A,
      /*provider=*/
      [&] {
        std::lock_guard<std::mutex> lk(serverMu);
        return Serializer::encodeState(serverCrdt);
      },
      /*consumer=*/[](const std::vector<uint8_t> &) {});

  server.startServer();

  CRDTEngine clientCrdt(2);
  std::mutex clientMu;

  StateSync client(
      SS_TCP_B,
      /*provider=*/[] { return std::vector<uint8_t>{}; },
      /*consumer=*/
      [&](const std::vector<uint8_t> &bytes) {
        CRDTEngine got = Serializer::decodeState(bytes);
        std::lock_guard<std::mutex> lk(clientMu);
        clientCrdt.loadState(std::move(got));
      });

  bool ok = client.requestState({"127.0.0.1:" + std::to_string(SS_TCP_A)});

  server.stopServer();

  assert(ok && "requestState should succeed");
  std::lock_guard<std::mutex> lk(clientMu);
  assert(clientCrdt.getDocument() == "hey");
  assert(clientCrdt.getSiteID() == 2 && "siteID preserved");
}

void test_pipeline_late_join() {
  // Node A: inserts text, serves state.
  PeerSocket sockA(SS_UDP_A);
  CRDTEngine crdtA(1);
  Pipeline pipeA(sockA, crdtA);

  StateSync serverA(
      SS_TCP_A, [&pipeA] { return pipeA.serializeState(); },
      [](const std::vector<uint8_t> &) {});
  serverA.startServer();
  pipeA.start();

  pipeA.localInsert(0, 'h');
  pipeA.localInsert(1, 'e');
  pipeA.localInsert(2, 'l');
  pipeA.localInsert(3, 'l');
  pipeA.localInsert(4, 'o');

  // Node B: starts with empty CRDT, requests state within 5 s.
  PeerSocket sockB(SS_UDP_B);
  CRDTEngine crdtB(2);
  Pipeline pipeB(sockB, crdtB);

  sockA.addPeer("127.0.0.1:" + std::to_string(SS_UDP_B));
  sockB.addPeer("127.0.0.1:" + std::to_string(SS_UDP_A));

  pipeB.start();

  auto t0 = std::chrono::steady_clock::now();
  bool ok = pipeB.syncState({"127.0.0.1:" + std::to_string(SS_TCP_A)}, SS_TCP_B,
                            /*timeoutPerPeerMs=*/3000);
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count();

  pipeA.stop();
  pipeB.stop();
  serverA.stopServer();

  assert(ok && "syncState must succeed");
  assert(elapsed < 5 && "state must arrive within 5 seconds");
  assert(pipeB.getDocument() == "hello");
}

void test_pipeline_ops_buffered_during_sync() {
  // A has one character; B will receive a second character while syncing.
  PeerSocket sockA(SS_UDP_A);
  CRDTEngine crdtA(1);
  Pipeline pipeA(sockA, crdtA);

  std::mutex serverMu;
  // The provider sleeps briefly to widen the race window.
  StateSync serverA(
      SS_TCP_A,
      [&pipeA] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return pipeA.serializeState();
      },
      [](const std::vector<uint8_t> &) {});
  serverA.startServer();

  sockA.addPeer("127.0.0.1:" + std::to_string(SS_UDP_B));

  PeerSocket sockB(SS_UDP_B);
  CRDTEngine crdtB(2);
  Pipeline pipeB(sockB, crdtB);
  sockB.addPeer("127.0.0.1:" + std::to_string(SS_UDP_A));

  pipeA.start();
  pipeB.start();

  pipeA.localInsert(0, 'a'); // inserted before sync starts

  // Kick off the sync in a background thread so we can insert concurrently.
  std::atomic<bool> syncDone{false};
  std::thread syncThread([&] {
    pipeB.syncState({"127.0.0.1:" + std::to_string(SS_TCP_A)}, SS_TCP_B);
    syncDone.store(true);
  });

  // Give the sync a moment to start, then insert a second character from A.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pipeA.localInsert(1, 'b'); // arrives at B while syncing (buffered)

  syncThread.join();

  // Wait for the buffered op to be applied.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (pipeB.getDocument().size() < 2 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

  pipeA.stop();
  pipeB.stop();
  serverA.stopServer();

  std::string doc = pipeB.getDocument();
  assert(doc.size() == 2 && "buffered op must be applied after sync");
  assert(doc == pipeA.getDocument() && "documents must converge");
}

// Opens a TCP listener, accepts one connection, sends partial data, closes.
static void runCrashServer(uint16_t port, std::atomic<bool> &ready) {
  int srv = ::socket(AF_INET, SOCK_STREAM, 0);
  int yes = 1;
  ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  sa.sin_port = htons(port);
  ::bind(srv, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa));
  ::listen(srv, 1);

  ready.store(true);

  int cli = ::accept(srv, nullptr, nullptr);
  if (cli >= 0) {
    // Send only 3 bytes of garbage — simulates crash mid-transfer.
    uint8_t partial[3] = {0x02, 0x00, 0x00};
    ::write(cli, partial, sizeof(partial));
    ::close(cli);
  }
  ::close(srv);
}

void test_state_sync_crash_retry() {
  // Good server with actual state.
  CRDTEngine serverCrdt(1);
  serverCrdt.localInsert(0, 'o');
  serverCrdt.localInsert(1, 'k');
  std::mutex serverMu;

  StateSync goodServer(
      SS_TCP_A,
      [&] {
        std::lock_guard<std::mutex> lk(serverMu);
        return Serializer::encodeState(serverCrdt);
      },
      [](const std::vector<uint8_t> &) {});
  goodServer.startServer();

  // Crash server: accepts, sends partial data, disconnects.
  std::atomic<bool> crashReady{false};
  std::thread crashThread([&] { runCrashServer(SS_TCP_CRASH, crashReady); });
  while (!crashReady.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  CRDTEngine clientCrdt(2);
  std::mutex clientMu;

  StateSync client(
      SS_TCP_B, [] { return std::vector<uint8_t>{}; },
      [&](const std::vector<uint8_t> &bytes) {
        CRDTEngine got = Serializer::decodeState(bytes);
        std::lock_guard<std::mutex> lk(clientMu);
        clientCrdt.loadState(std::move(got));
      });

  // Try crash server first (will fail), then good server.
  bool ok = client.requestState({"127.0.0.1:" + std::to_string(SS_TCP_CRASH),
                                 "127.0.0.1:" + std::to_string(SS_TCP_A)},
                                /*timeoutPerPeerMs=*/2000);

  goodServer.stopServer();
  crashThread.join();

  assert(ok && "retry after crash must succeed");
  std::lock_guard<std::mutex> lk(clientMu);
  assert(clientCrdt.getDocument() == "ok");
}

void test_state_sync_unreachable_fallback() {
  CRDTEngine serverCrdt(1);
  serverCrdt.localInsert(0, 'z');
  std::mutex serverMu;

  StateSync goodServer(
      SS_TCP_A,
      [&] {
        std::lock_guard<std::mutex> lk(serverMu);
        return Serializer::encodeState(serverCrdt);
      },
      [](const std::vector<uint8_t> &) {});
  goodServer.startServer();

  CRDTEngine clientCrdt(2);
  std::mutex clientMu;

  StateSync client(
      SS_TCP_B, [] { return std::vector<uint8_t>{}; },
      [&](const std::vector<uint8_t> &bytes) {
        CRDTEngine got = Serializer::decodeState(bytes);
        std::lock_guard<std::mutex> lk(clientMu);
        clientCrdt.loadState(std::move(got));
      });

  // Port 6299 has no listener (connection refused immediately).
  bool ok = client.requestState({"127.0.0.1:6299", // unreachable
                                 "127.0.0.1:" + std::to_string(SS_TCP_A)},
                                /*timeoutPerPeerMs=*/2000);

  goodServer.stopServer();

  assert(ok && "fallback to second peer must succeed");
  std::lock_guard<std::mutex> lk(clientMu);
  assert(clientCrdt.getDocument() == "z");
}

void run_state_sync_tests() {
  test_load_state_replaces_document();
  test_load_state_clock_advances();
  test_state_sync_server_client();
  test_pipeline_late_join();
  test_pipeline_ops_buffered_during_sync();
  test_state_sync_crash_retry();
  test_state_sync_unreachable_fallback();
}
