#include "rga.h"
#include <cassert>

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

void run_rga_tests() {
  test_constructor();
  test_local_insert();
  test_local_delete();
  test_apply_remote_insert();
  test_apply_remote_delete();
  test_get_document();
  test_get_site_id();
  test_get_lamport_clock();
}
