#include "rga.h"
#include "serializer.h"
#include <cassert>
#include <stdexcept>

static bool opEqual(const Operation &a, const Operation &b) {
  return a.type == b.type && a.id == b.id &&
         a.leftNeighborID == b.leftNeighborID && a.value == b.value;
}

void test_encode_decode_insert() {
  Operation op;
  op.type = OpType::INSERT;
  op.id = CharID(7, 3);
  op.leftNeighborID = CharID(0, 0);
  op.value = 'x';

  auto bytes = Serializer::encode(op);
  auto result = Serializer::decode(bytes);

  assert(opEqual(op, result));
}

void test_encode_decode_delete() {
  Operation op;
  op.type = OpType::DELETE;
  op.id = CharID(42, 99);
  op.leftNeighborID = CharID(41, 99);
  op.value = '\0'; // value is irrelevant for DELETE

  auto bytes = Serializer::encode(op);
  auto result = Serializer::decode(bytes);

  assert(result.type == OpType::DELETE);
  assert(result.id == op.id);
  assert(result.leftNeighborID == op.leftNeighborID);
}

void test_encode_decode_large_ids() {
  // Test with large (and negative) clock/siteID values to verify endianness
  Operation op;
  op.type = OpType::INSERT;
  op.id = CharID(0x7FFFFFFF, -1);
  op.leftNeighborID = CharID(-2147483648, 0x7FFFFFFF);
  op.value = '\n';

  auto bytes = Serializer::encode(op);
  auto result = Serializer::decode(bytes);

  assert(opEqual(op, result));
}

void test_message_framing_operation() {
  Operation op;
  op.type = OpType::INSERT;
  op.id = CharID(1, 1);
  op.leftNeighborID = SENTINEL_ID;
  op.value = 'a';

  auto bytes = Serializer::encode(op);

  // [0]: MsgType::OPERATION = 0x01
  assert(bytes[0] == 0x01);

  // [1..4]: payload length = 18 (big-endian)
  uint32_t len = (static_cast<uint32_t>(bytes[1]) << 24) |
                 (static_cast<uint32_t>(bytes[2]) << 16) |
                 (static_cast<uint32_t>(bytes[3]) << 8) |
                 static_cast<uint32_t>(bytes[4]);
  assert(len == 18);
  assert(bytes.size() == 1 + 4 + 18);
}

void test_decode_wrong_msg_type_throws() {
  Operation op;
  op.type = OpType::INSERT;
  op.id = CharID(1, 1);
  op.leftNeighborID = SENTINEL_ID;
  op.value = 'z';

  auto bytes = Serializer::encode(op);
  // Corrupt the type byte to STATE
  bytes[0] = static_cast<uint8_t>(MsgType::STATE);

  bool threw = false;
  try {
    Serializer::decode(bytes);
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw);
}

void test_decode_truncated_throws() {
  Operation op;
  op.type = OpType::INSERT;
  op.id = CharID(1, 1);
  op.leftNeighborID = SENTINEL_ID;
  op.value = 'z';

  auto bytes = Serializer::encode(op);
  bytes.resize(bytes.size() - 5); // truncate

  bool threw = false;
  try {
    Serializer::decode(bytes);
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw);
}

void test_encode_decode_state_empty() {
  CRDTEngine engine(5);

  auto bytes = Serializer::encodeState(engine);
  auto rebuilt = Serializer::decodeState(bytes);

  assert(rebuilt.getSiteID() == engine.getSiteID());
  assert(rebuilt.getLamportClock() == engine.getLamportClock());
  assert(rebuilt.getDocument() == engine.getDocument());
}

void test_encode_decode_state_with_content() {
  CRDTEngine engine(2);
  engine.localInsert(0, 'h');
  engine.localInsert(1, 'e');
  engine.localInsert(2, 'l');
  engine.localInsert(3, 'l');
  engine.localInsert(4, 'o');

  auto bytes = Serializer::encodeState(engine);
  auto rebuilt = Serializer::decodeState(bytes);

  assert(rebuilt.getSiteID() == engine.getSiteID());
  assert(rebuilt.getLamportClock() == engine.getLamportClock());
  assert(rebuilt.getDocument() == "hello");
}

void test_encode_decode_state_with_tombstones() {
  CRDTEngine engine(1);
  engine.localInsert(0, 'a');
  engine.localInsert(1, 'b');
  engine.localInsert(2, 'c');
  engine.localDelete(1); // delete 'b', leaving "ac"

  auto bytes = Serializer::encodeState(engine);
  auto rebuilt = Serializer::decodeState(bytes);

  assert(rebuilt.getDocument() == "ac");
  assert(rebuilt.getSiteID() == 1);
}

void test_encode_decode_state_clock_preserved() {
  CRDTEngine engine(10);
  engine.localInsert(0, 'X');
  engine.localInsert(1, 'Y');
  int clockBefore = engine.getLamportClock();

  auto bytes = Serializer::encodeState(engine);
  auto rebuilt = Serializer::decodeState(bytes);

  assert(rebuilt.getLamportClock() == clockBefore);
}

void test_message_framing_state() {
  CRDTEngine engine(1);
  engine.localInsert(0, 'a');

  auto bytes = Serializer::encodeState(engine);

  // [0]: MsgType::STATE = 0x02
  assert(bytes[0] == 0x02);

  // [1..4]: payload length (big-endian)
  uint32_t len = (static_cast<uint32_t>(bytes[1]) << 24) |
                 (static_cast<uint32_t>(bytes[2]) << 16) |
                 (static_cast<uint32_t>(bytes[3]) << 8) |
                 static_cast<uint32_t>(bytes[4]);
  // header(12) + 1 node * 18 bytes = 30
  assert(len == 30);
  assert(bytes.size() == 1 + 4 + 30);
}

void test_decodeState_wrong_msg_type_throws() {
  CRDTEngine engine(1);
  auto bytes = Serializer::encodeState(engine);
  bytes[0] = static_cast<uint8_t>(MsgType::OPERATION);

  bool threw = false;
  try {
    Serializer::decodeState(bytes);
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw);
}

void test_rebuilt_engine_accepts_new_operations() {
  // After reconstructing from state, the engine should still work correctly
  CRDTEngine engine(3);
  engine.localInsert(0, 'f');
  engine.localInsert(1, 'o');
  engine.localInsert(2, 'o');

  auto rebuilt = Serializer::decodeState(Serializer::encodeState(engine));
  rebuilt.localInsert(3, '!');
  assert(rebuilt.getDocument() == "foo!");
  rebuilt.localDelete(0);
  assert(rebuilt.getDocument() == "oo!");
}

void test_rebuilt_engine_applies_remote_ops() {
  // Site1 inserts 'a', site2 applies it, then gets serialized.
  // After reconstruction site2 should still accept further ops from site1
  // that reference nodes already present in its state.
  CRDTEngine site1(1);
  Operation op_a = site1.localInsert(0, 'a');

  CRDTEngine site2(2);
  site2.applyRemote(op_a);
  assert(site2.getDocument() == "a");

  auto rebuilt = Serializer::decodeState(Serializer::encodeState(site2));
  assert(rebuilt.getDocument() == "a");

  // site1 inserts 'b' after 'a'; 'a' is in rebuilt's state so this is safe
  Operation op_b = site1.localInsert(1, 'b');
  rebuilt.applyRemote(op_b);
  assert(rebuilt.getDocument() == "ab");
}

void run_serializer_tests() {
  test_encode_decode_insert();
  test_encode_decode_delete();
  test_encode_decode_large_ids();
  test_message_framing_operation();
  test_decode_wrong_msg_type_throws();
  test_decode_truncated_throws();

  test_encode_decode_state_empty();
  test_encode_decode_state_with_content();
  test_encode_decode_state_with_tombstones();
  test_encode_decode_state_clock_preserved();
  test_message_framing_state();
  test_decodeState_wrong_msg_type_throws();
  test_rebuilt_engine_accepts_new_operations();
  test_rebuilt_engine_applies_remote_ops();
}
