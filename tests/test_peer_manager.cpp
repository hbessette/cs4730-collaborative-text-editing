#include "peer_manager.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static const uint16_t PM_PORT_A = 5700;
static const uint16_t PM_PORT_B = 5701;
static const uint16_t PM_PORT_C = 5702;

void test_generate_site_id_unique() {
  uint32_t a = generateSiteID();
  uint32_t b = generateSiteID();
  assert(a != 0 && b != 0);
  assert(a != b);
}

void test_peer_manager_start_stop() {
  PeerManager pm(PM_PORT_C, 42);
  assert(!pm.isRunning());
  pm.start();
  assert(pm.isRunning());
  pm.stop();
  assert(!pm.isRunning());
  assert(pm.activePeers().empty());
}

void test_peer_manager_join_discovery() {
  int pfd[2];
  assert(pipe(pfd) == 0);

  pid_t pid = fork();
  assert(pid >= 0);

  if (pid == 0) {
    close(pfd[0]);

    std::atomic<bool> joinFired{false};
    PeerManager pm(PM_PORT_A, 1);
    pm.setOnPeerJoin([&](uint32_t id, const std::string &) {
      if (id == 2)
        joinFired.store(true);
    });
    pm.start();

    char sig = 1;
    write(pfd[1], &sig, 1);
    close(pfd[1]);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!joinFired.load() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

    pm.stop();
    _exit(joinFired.load() ? 0 : 1);

  } else {
    close(pfd[1]);
    char sig;
    read(pfd[0], &sig, 1);
    close(pfd[0]);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::atomic<bool> joinFired{false};
    PeerManager pm(PM_PORT_B, 2);
    pm.setOnPeerJoin([&](uint32_t id, const std::string &) {
      if (id == 1)
        joinFired.store(true);
    });
    pm.addKnownPeer("127.0.0.1:" + std::to_string(PM_PORT_A));
    pm.start();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!joinFired.load() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

    pm.stop();

    int status;
    waitpid(pid, &status, 0);
    int childExit = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    assert(joinFired.load() && "parent: onPeerJoin never fired for peer A");
    assert(childExit == 0 && "child:  onPeerJoin never fired for peer B");
  }
}

void test_peer_manager_leave() {
  int pfd[2];
  assert(pipe(pfd) == 0);

  pid_t pid = fork();
  assert(pid >= 0);

  if (pid == 0) {
    close(pfd[0]);
    PeerManager pm(PM_PORT_A, 1);
    pm.addKnownPeer("127.0.0.1:" + std::to_string(PM_PORT_B));
    pm.start();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (pm.activePeers().empty() &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

    char sig = 1;
    write(pfd[1], &sig, 1);
    close(pfd[1]);
    pm.stop();
    _exit(0);

  } else {
    close(pfd[1]);

    std::atomic<bool> leaveFired{false};
    PeerManager pm(PM_PORT_B, 2);
    pm.setOnPeerLeave([&](uint32_t id, const std::string &) {
      if (id == 1)
        leaveFired.store(true);
    });
    pm.start();

    char sig;
    read(pfd[0], &sig, 1);
    close(pfd[0]);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!leaveFired.load() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

    pm.stop();

    int status;
    waitpid(pid, &status, 0);
    assert(leaveFired.load() && "onPeerLeave never fired after LEAVE message");
  }
}

void run_peer_manager_tests() {
  test_generate_site_id_unique();
  test_peer_manager_start_stop();
  test_peer_manager_join_discovery();
  test_peer_manager_leave();
}
