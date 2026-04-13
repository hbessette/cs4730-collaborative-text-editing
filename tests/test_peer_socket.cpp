#include "peer_socket.h"
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static const uint16_t BASE_PORT = 5600;

// Timeout path
void test_peer_socket_receive_timeout() {
  PeerSocket sock(BASE_PORT);
  bool threw = false;
  try {
    sock.receive(100);
  } catch (const std::runtime_error &e) {
    threw = (std::strstr(e.what(), "timeout") != nullptr);
  }
  assert(threw && "receive() should throw on timeout");
}

// Two-process unicast round-trip via loopback.
void test_peer_socket_send_receive() {
  int ready_pipe[2];
  assert(pipe(ready_pipe) == 0);

  pid_t pid = fork();
  assert(pid >= 0);

  if (pid == 0) {
    close(ready_pipe[0]);
    int code = 1;
    try {
      PeerSocket sock(static_cast<uint16_t>(BASE_PORT + 1));
      char sig = 1;
      write(ready_pipe[1], &sig, 1);
      close(ready_pipe[1]);

      std::pair<std::vector<uint8_t>, std::string> result = sock.receive(5000);
      std::vector<uint8_t> expected = {'p', '2', 'p'};
      if (result.first == expected)
        code = 0;
    } catch (...) {
      close(ready_pipe[1]);
    }
    _exit(code);

  } else {
    close(ready_pipe[1]);
    char sig;
    ssize_t n = read(ready_pipe[0], &sig, 1);
    close(ready_pipe[0]);
    assert(n == 1);
    usleep(10000);

    PeerSocket sock(BASE_PORT);
    sock.addPeer("127.0.0.1:" + std::to_string(BASE_PORT + 1));

    std::vector<uint8_t> msg = {'p', '2', 'p'};
    sock.send(msg);

    int status = 0;
    waitpid(pid, &status, 0);
    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    assert(exitCode == 0 && "PeerSocket round-trip failed");
  }
}

// addPeer with bare IP (no port) should default to the socket's own port.
void test_peer_socket_bare_ip() {
  PeerSocket sock(static_cast<uint16_t>(BASE_PORT + 2));

  sock.addPeer("127.0.0.1");

  std::vector<uint8_t> msg = {'o', 'k'};
  sock.send(msg);

  auto result = sock.receive(1000);
  assert(result.first == msg && "bare-IP addPeer did not default to socket's own port");
}

// addPeer with invalid address should throw.
void test_peer_socket_invalid_addr_throws() {
  PeerSocket sock(static_cast<uint16_t>(BASE_PORT + 3));
  bool threw = false;
  try {
    sock.addPeer("not_an_ip");
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw && "addPeer should throw on invalid address");
}

void run_peer_socket_tests() {
  test_peer_socket_receive_timeout();
  test_peer_socket_send_receive();
  test_peer_socket_bare_ip();
  test_peer_socket_invalid_addr_throws();
}
