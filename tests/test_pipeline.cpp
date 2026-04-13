#include "peer_socket.h"
#include "pipeline.h"
#include "rga.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

static const uint16_t PL_PORT_A = 6000;
static const uint16_t PL_PORT_B = 6001;
static const uint16_t PL_PORT_C = 6002;

void test_mpsc_queue_push_pop() {
  std::atomic<bool> stop{false};
  MpscQueue<int> q;

  q.push(1);
  q.push(2);
  q.push(3);

  int val = 0;
  assert(q.pop(val, stop) && val == 1);
  assert(q.pop(val, stop) && val == 2);
  assert(q.pop(val, stop) && val == 3);
}

void test_mpsc_queue_stop_exits_immediately() {
  std::atomic<bool> stop{true};
  MpscQueue<int> q;
  q.push(42);

  int val = 0;
  bool got = q.pop(val, stop);
  assert(!got && "pop should return false when stop is already set");
}

void test_mpsc_queue_stop_wakes_blocked_consumer() {
  std::atomic<bool> stop{false};
  MpscQueue<int> q;
  std::atomic<bool> returned{false};

  std::thread consumer([&] {
    int val = 0;
    q.pop(val, stop);
    returned.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  assert(!returned.load());

  stop.store(true);
  q.wake();

  consumer.join();
  assert(returned.load());
}

void test_mpsc_queue_concurrent_producers() {
  std::atomic<bool> stop{false};
  MpscQueue<int> q;
  const int N = 100;

  std::vector<std::thread> producers;
  for (int t = 0; t < 4; ++t) {
    producers.emplace_back([&, t] {
      for (int i = 0; i < N; ++i)
        q.push(t * N + i);
    });
  }
  for (auto &p : producers)
    p.join();

  int count = 0;
  int val = 0;
  while (true) {
    stop.store(false);
    if (!q.pop(val, stop))
      break;
    ++count;
    if (count == 4 * N)
      break;
  }
  stop.store(false);
  assert(count == 4 * N);
}

void test_pipeline_start_stop() {
  PeerSocket sock(PL_PORT_C);
  CRDTEngine crdt(1);
  Pipeline pipeline(sock, crdt);

  assert(!pipeline.isRunning());
  pipeline.start();
  assert(pipeline.isRunning());
  pipeline.stop();
  assert(!pipeline.isRunning());
}

void test_pipeline_local_ops_are_applied() {
  PeerSocket sock(PL_PORT_C);
  CRDTEngine crdt(1);
  Pipeline pipeline(sock, crdt);

  pipeline.start();
  pipeline.localInsert(0, 'h');
  pipeline.localInsert(1, 'i');
  std::string doc = pipeline.getDocument();
  pipeline.stop();

  assert(doc == "hi");
}

void test_pipeline_local_ops_thread_safety() {
  PeerSocket sock(PL_PORT_C);
  CRDTEngine crdt(1);
  Pipeline pipeline(sock, crdt);
  pipeline.start();

  const int THREADS = 4;
  const int OPS_PER_THREAD = 25;
  std::vector<std::thread> workers;

  for (int t = 0; t < THREADS; ++t) {
    workers.emplace_back([&] {
      for (int i = 0; i < OPS_PER_THREAD; ++i)
        pipeline.localInsert(0, 'a');
    });
  }
  for (auto &w : workers)
    w.join();

  std::string doc = pipeline.getDocument();
  pipeline.stop();

  assert(doc.size() == static_cast<size_t>(THREADS * OPS_PER_THREAD));
}

void test_pipeline_end_to_end() {
  PeerSocket sockA(PL_PORT_A);
  PeerSocket sockB(PL_PORT_B);

  sockA.addPeer("127.0.0.1:" + std::to_string(PL_PORT_B));
  sockB.addPeer("127.0.0.1:" + std::to_string(PL_PORT_A));

  CRDTEngine crdtA(1);
  CRDTEngine crdtB(2);

  Pipeline pipeA(sockA, crdtA);
  Pipeline pipeB(sockB, crdtB);

  std::atomic<bool> opReceived{false};
  std::mutex cvMu;
  std::condition_variable cv;

  pipeB.setOnRemoteOp([&](const Operation &) {
    opReceived.store(true);
    cv.notify_one();
  });

  pipeA.start();
  pipeB.start();

  pipeA.localInsert(0, 'x');

  {
    std::unique_lock<std::mutex> lk(cvMu);
    cv.wait_for(lk, std::chrono::seconds(2), [&] { return opReceived.load(); });
  }

  pipeA.stop();
  pipeB.stop();

  assert(opReceived.load() && "remote op never arrived at node B");
  assert(pipeB.getDocument() == "x");
  assert(pipeA.getDocument() == pipeB.getDocument());
}

void test_pipeline_bidirectional() {
  PeerSocket sockA(PL_PORT_A);
  PeerSocket sockB(PL_PORT_B);

  sockA.addPeer("127.0.0.1:" + std::to_string(PL_PORT_B));
  sockB.addPeer("127.0.0.1:" + std::to_string(PL_PORT_A));

  CRDTEngine crdtA(1);
  CRDTEngine crdtB(2);

  Pipeline pipeA(sockA, crdtA);
  Pipeline pipeB(sockB, crdtB);

  std::atomic<int> opsAtA{0};
  std::atomic<int> opsAtB{0};
  std::mutex cvMu;
  std::condition_variable cv;

  pipeA.setOnRemoteOp([&](const Operation &) {
    ++opsAtA;
    cv.notify_one();
  });
  pipeB.setOnRemoteOp([&](const Operation &) {
    ++opsAtB;
    cv.notify_one();
  });

  pipeA.start();
  pipeB.start();

  pipeA.localInsert(0, 'a');
  pipeB.localInsert(0, 'b');

  {
    std::unique_lock<std::mutex> lk(cvMu);
    cv.wait_for(lk, std::chrono::seconds(3),
                [&] { return opsAtA.load() >= 1 && opsAtB.load() >= 1; });
  }

  pipeA.stop();
  pipeB.stop();

  assert(opsAtA.load() >= 1 && "A never received B's op");
  assert(opsAtB.load() >= 1 && "B never received A's op");

  assert(pipeA.getDocument().size() == 2);
  assert(pipeB.getDocument().size() == 2);
  assert(pipeA.getDocument() == pipeB.getDocument());
}

void run_pipeline_tests() {
  test_mpsc_queue_push_pop();
  test_mpsc_queue_stop_exits_immediately();
  test_mpsc_queue_stop_wakes_blocked_consumer();
  test_mpsc_queue_concurrent_producers();
  test_pipeline_start_stop();
  test_pipeline_local_ops_are_applied();
  test_pipeline_local_ops_thread_safety();
  test_pipeline_end_to_end();
  test_pipeline_bidirectional();
}
