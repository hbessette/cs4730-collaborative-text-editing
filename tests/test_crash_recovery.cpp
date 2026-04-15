// test_crash_recovery.cpp
//
// Verifies that after a peer is SIGKILL-ed mid-edit, the surviving peer:
//   1. Detects the crash via heartbeat timeout (leave callback fires).
//   2. Continues editing without disruption (local ops still apply cleanly).

#include "peer_manager.h"
#include "peer_socket.h"
#include "pipeline.h"
#include "rga.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// Ports used by this test suite — chosen to avoid conflicts with other suites.
// Layout: heartbeat on CR_HB_*, data on CR_DATA_*.
static const uint16_t CR_HB_PARENT = 5800;
static const uint16_t CR_HB_CHILD  = 5801;
static const uint16_t CR_DATA_PARENT = 5810;

// SIGKILL a peer mid-edit and verify the surviving peer converges.
//
// Scenario:
//   child  = "victim" peer running only a PeerManager (sends heartbeats)
//   parent = surviving peer with PeerManager + Pipeline
//
// Steps:
//   1. Child starts, signals parent it is ready.
//   2. Parent waits for the join callback (child discovered via heartbeat).
//   3. Parent applies 5 local inserts.
//   4. Parent SIGKILLs the child.
//   5. Parent waits up to (DEAD_THRESHOLD_MS + 2 s) for the leave callback.
//   6. Parent applies 3 more local inserts.
//   7. Asserts: leave callback fired; document contains all 8 chars.
void test_sigkill_convergence() {
  int pfd[2]; // pipe: child signals parent when ready
  assert(pipe(pfd) == 0);

  pid_t childPid = fork();
  assert(childPid >= 0);

  if (childPid == 0) {
    // ---- child (victim) ----
    close(pfd[0]);

    PeerManager pm(CR_HB_CHILD, /*siteID=*/999);
    pm.addKnownPeer("127.0.0.1:" + std::to_string(CR_HB_PARENT));
    pm.start();

    // Signal parent we are up and sending heartbeats.
    char sig = 1;
    write(pfd[1], &sig, 1);
    close(pfd[1]);

    // Stay alive until SIGKILLed.
    while (true)
      std::this_thread::sleep_for(std::chrono::seconds(60));

    // Unreachable, but tidy up for analysis tools.
    pm.stop();
    _exit(0);

  } else {
    // ---- parent (survivor) ----
    close(pfd[1]);

    // Wait for child to start.
    char sig;
    read(pfd[0], &sig, 1);
    close(pfd[0]);

    std::atomic<bool> joinFired{false};
    std::atomic<bool> leaveFired{false};

    PeerSocket dataSocket(CR_DATA_PARENT);
    CRDTEngine crdt(1);
    Pipeline pipeline(dataSocket, crdt);
    pipeline.addKnownSiteID(1); // own site

    PeerManager peerMgr(CR_HB_PARENT, /*siteID=*/1);
    peerMgr.addKnownPeer("127.0.0.1:" + std::to_string(CR_HB_CHILD));

    peerMgr.setOnPeerJoin([&](uint32_t id, const std::string &) {
      if (id == 999)
        joinFired.store(true);
    });
    peerMgr.setOnPeerLeave([&](uint32_t id, const std::string &) {
      if (id == 999)
        leaveFired.store(true);
    });

    peerMgr.start();
    pipeline.start();

    // Wait for the child to be discovered via heartbeat.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!joinFired.load() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(joinFired.load() && "child peer was never discovered via heartbeat");

    // Apply 5 local inserts while child is alive.
    for (char c : {'h', 'e', 'l', 'l', 'o'})
      pipeline.localInsert(static_cast<int>(pipeline.getDocument().size()), c);
    assert(pipeline.getDocument() == "hello");

    // Kill the child abruptly (simulates a crash — no LEAVE message sent).
    kill(childPid, SIGKILL);
    waitpid(childPid, nullptr, 0);

    // Wait for heartbeat timeout to expire and the leave callback to fire.
    // DEAD_THRESHOLD_MS is 6 000 ms; allow 8 s total.
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!leaveFired.load() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    assert(leaveFired.load() && "leave callback never fired after SIGKILL");

    // Surviving peer continues editing without disruption.
    for (char c : {' ', 'o', 'k'})
      pipeline.localInsert(static_cast<int>(pipeline.getDocument().size()), c);
    assert(pipeline.getDocument() == "hello ok");

    pipeline.stop();
    peerMgr.stop();
  }
}

void run_crash_recovery_tests() {
  test_sigkill_convergence();
}
