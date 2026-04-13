#include "rga.h"
#include <cassert>
#include <vector>

void test_constructor() {
  CRDTEngine crdt(1);
  assert(crdt.getSiteID() == 1);
  assert(crdt.getLamportClock() == 0);
  assert(crdt.getDocument() == "");
}

void test_local_insert() {
  CRDTEngine crdt(1);
  Operation op = crdt.localInsert(0, 'a');
  assert(op.type == OpType::INSERT);
  assert(op.id == CharID(1, 1));
  assert(op.leftNeighborID == CharID(0, 0));
  assert(op.value == 'a');
  assert(crdt.getDocument() == "a");
  crdt.localInsert(0, 'b');
  assert(crdt.getDocument() == "ba");
  crdt.localInsert(2, 'c');
  assert(crdt.getDocument() == "bac");
  crdt.localInsert(1, 'd');
  assert(crdt.getDocument() == "bdac");
}

void test_local_delete() {
  CRDTEngine crdt(1);
  crdt.localInsert(0, 'a');
  crdt.localInsert(1, 'b');
  crdt.localInsert(2, 'c');
  Operation op = crdt.localDelete(1);
  assert(op.type == OpType::DELETE);
  assert(crdt.getDocument() == "ac");
  crdt.localDelete(0);
  assert(crdt.getDocument() == "c");
  crdt.localDelete(0);
  assert(crdt.getDocument() == "");
}

void test_apply_remote_insert() {
  CRDTEngine site1(1);
  CRDTEngine site2(2);
  Operation op = site1.localInsert(0, 'a');
  site2.applyRemote(op);
  assert(site2.getDocument() == "a");
  Operation op1 = site1.localInsert(1, 'b');
  Operation op2 = site2.localInsert(1, 'c');
  site1.applyRemote(op2);
  site2.applyRemote(op1);
  assert(site1.getDocument() == site2.getDocument());
}

void test_apply_remote_delete() {
  CRDTEngine site1(1);
  CRDTEngine site2(2);
  Operation insertOp = site1.localInsert(0, 'a');
  site2.applyRemote(insertOp);
  Operation deleteOp = site1.localDelete(0);
  site2.applyRemote(deleteOp);
  assert(site1.getDocument() == "");
  assert(site2.getDocument() == "");
  // idempotency: applying the same delete again is a no-op
  site2.applyRemote(deleteOp);
  assert(site2.getDocument() == "");
}

void test_get_document() {
  CRDTEngine crdt(1);
  assert(crdt.getDocument() == "");
  crdt.localInsert(0, 'h');
  crdt.localInsert(1, 'i');
  assert(crdt.getDocument() == "hi");
  crdt.localDelete(0);
  assert(crdt.getDocument() == "i");
}

void test_get_site_id() {
  CRDTEngine crdt(42);
  assert(crdt.getSiteID() == 42);
}

void test_get_lamport_clock() {
  CRDTEngine crdt(1);
  assert(crdt.getLamportClock() == 0);
  crdt.localInsert(0, 'a');
  assert(crdt.getLamportClock() == 1);
  crdt.localDelete(0);
  assert(crdt.getLamportClock() == 2);
}

// Two sites insert at the same position concurrently.
// RGA tiebreaks by CharID: higher siteID wins (placed first in sequence).
// Both sites must converge to the same deterministic document.
void test_concurrent_insert_same_position() {
  CRDTEngine site1(1), site2(2);

  Operation op1 = site1.localInsert(0, 'a'); // CharID {1,1}
  Operation op2 = site2.localInsert(0, 'b'); // CharID {1,2}

  site1.applyRemote(op2);
  site2.applyRemote(op1);

  // siteID=2 > siteID=1 at equal clock, so 'b' precedes 'a'
  assert(site1.getDocument() == "ba");
  assert(site2.getDocument() == "ba");
}

// Both sites delete the same character concurrently.
// The second delete is a no-op (tombstone already set); document must be empty
// on both.
void test_concurrent_delete_same_character() {
  CRDTEngine site1(1), site2(2);

  Operation insertOp = site1.localInsert(0, 'x');
  site2.applyRemote(insertOp);
  // both have "x"

  Operation del1 = site1.localDelete(0);
  Operation del2 = site2.localDelete(0);

  site1.applyRemote(del2);
  site2.applyRemote(del1);

  assert(site1.getDocument() == "");
  assert(site2.getDocument() == "");
}

// site1 inserts 'b' after 'a' while site2 concurrently deletes 'a'.
// After merging, 'b' must still appear on both sites because its left neighbor
// ('a') remains in the sequence as a tombstone and 'b' was anchored to it.
void test_insert_after_deleted_character() {
  CRDTEngine site1(1), site2(2);

  Operation op_a = site1.localInsert(0, 'a');
  site2.applyRemote(op_a);
  // both have "a"

  // concurrent: site2 deletes 'a', site1 inserts 'b' after 'a'
  Operation op_del = site2.localDelete(0);
  Operation op_b = site1.localInsert(1, 'b'); // left neighbor = 'a'

  site1.applyRemote(op_del); // 'a' tombstoned; 'b' still present
  site2.applyRemote(op_b);   // 'b' inserted after tombstoned 'a'

  assert(site1.getDocument() == "b");
  assert(site2.getDocument() == "b");
}

// 100 randomised insert/delete operations across 3 independent sites.
// Each site's log is replayed (in generation order) to the other two.
// All three sites must converge to the same document.
void test_randomized_convergence() {
  // Deterministic LCG so the test is reproducible
  unsigned rng = 42;
  auto rand_int = [&](int mod) -> int {
    rng = rng * 1664525u + 1013904223u;
    return static_cast<int>((rng >> 16) % static_cast<unsigned>(mod));
  };

  CRDTEngine sites[3] = {CRDTEngine(1), CRDTEngine(2), CRDTEngine(3)};
  std::vector<Operation> logs[3];

  for (int i = 0; i < 100; i++) {
    int s = rand_int(3);
    int sz = static_cast<int>(sites[s].getDocument().size());
    if (sz > 0 && rand_int(4) == 0) {
      logs[s].push_back(sites[s].localDelete(rand_int(sz)));
    } else {
      char c = static_cast<char>('a' + rand_int(26));
      logs[s].push_back(sites[s].localInsert(rand_int(sz + 1), c));
    }
  }

  // Replay each site's log (in generation order) to the other two sites.
  // Causal order is preserved within each log because ops reference only
  // CharIDs created by the same site at lower clocks.
  for (int s = 0; s < 3; s++) {
    for (int other = 0; other < 3; other++) {
      if (other == s)
        continue;
      for (const Operation &op : logs[other]) {
        sites[s].applyRemote(op);
      }
    }
  }

  assert(sites[0].getDocument() == sites[1].getDocument());
  assert(sites[1].getDocument() == sites[2].getDocument());
}

// Ops from two sites applied in opposite orders must yield the same document.
// engine_A receives site1's ops then site2's; engine_B receives site2's then
// site1's.
void test_commutativity() {
  CRDTEngine site1(1), site2(2);
  std::vector<Operation> log1, log2;

  log1.push_back(site1.localInsert(0, 'x'));
  log1.push_back(site1.localInsert(1, 'y'));

  log2.push_back(site2.localInsert(0, 'a'));
  log2.push_back(site2.localInsert(1, 'b'));

  CRDTEngine engine_A(0);
  for (const Operation &op : log1)
    engine_A.applyRemote(op);
  for (const Operation &op : log2)
    engine_A.applyRemote(op);

  CRDTEngine engine_B(0);
  for (const Operation &op : log2)
    engine_B.applyRemote(op);
  for (const Operation &op : log1)
    engine_B.applyRemote(op);

  assert(engine_A.getDocument() == engine_B.getDocument());
}

void run_rga_tests() {
  test_constructor();
  test_local_insert();
  test_local_delete();
  test_apply_remote_insert();
  test_apply_remote_delete();
  test_get_document();
  test_get_site_id();
  test_get_lamport_clock();
  test_concurrent_insert_same_position();
  test_concurrent_delete_same_character();
  test_insert_after_deleted_character();
  test_randomized_convergence();
  test_commutativity();
}
