#include "serializer.h"
#include <stdexcept>

void Serializer::writeUint16(std::vector<uint8_t> &buf, uint16_t val) {
  buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>( val       & 0xFF));
}

uint16_t Serializer::readUint16(const uint8_t *data, size_t &offset) {
  uint16_t val = static_cast<uint16_t>(
      (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
  offset += 2;
  return val;
}

void Serializer::writeUint32(std::vector<uint8_t> &buf, uint32_t val) {
  buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

uint32_t Serializer::readUint32(const uint8_t *data, size_t &offset) {
  uint32_t val = (static_cast<uint32_t>(data[offset]) << 24) |
                 (static_cast<uint32_t>(data[offset + 1]) << 16) |
                 (static_cast<uint32_t>(data[offset + 2]) << 8) |
                 static_cast<uint32_t>(data[offset + 3]);
  offset += 4;
  return val;
}

void Serializer::writeInt32(std::vector<uint8_t> &buf, int32_t val) {
  writeUint32(buf, static_cast<uint32_t>(val));
}

int32_t Serializer::readInt32(const uint8_t *data, size_t &offset) {
  return static_cast<int32_t>(readUint32(data, offset));
}

static std::vector<uint8_t> buildFrame(MsgType msgType,
                                       const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> frame;
  frame.reserve(1 + 4 + payload.size());

  // type byte
  frame.push_back(static_cast<uint8_t>(msgType));

  // payload length (big-endian uint32)
  uint32_t len = static_cast<uint32_t>(payload.size());
  frame.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
  frame.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
  frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(len & 0xFF));

  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

std::vector<uint8_t> Serializer::encode(const Operation &op) {
  std::vector<uint8_t> payload;
  payload.reserve(18);

  // OpType byte: 0 = INSERT, 1 = DELETE
  payload.push_back(op.type == OpType::INSERT ? 0x00 : 0x01);

  writeInt32(payload, static_cast<int32_t>(op.id.clock));
  writeInt32(payload, static_cast<int32_t>(op.id.siteID));
  writeInt32(payload, static_cast<int32_t>(op.leftNeighborID.clock));
  writeInt32(payload, static_cast<int32_t>(op.leftNeighborID.siteID));

  payload.push_back(static_cast<uint8_t>(op.value));

  return buildFrame(MsgType::OPERATION, payload);
}

Operation Serializer::decode(const std::vector<uint8_t> &bytes) {
  if (bytes.size() < 5) {
    throw std::runtime_error("Serializer::decode: buffer too short for header");
  }

  const uint8_t *data = bytes.data();
  size_t offset = 0;

  uint8_t msgTypeByte = data[offset++];
  if (static_cast<MsgType>(msgTypeByte) != MsgType::OPERATION) {
    throw std::runtime_error(
        "Serializer::decode: expected OPERATION message type");
  }

  uint32_t payloadLen = readUint32(data, offset);
  if (bytes.size() - offset < payloadLen) {
    throw std::runtime_error(
        "Serializer::decode: buffer too short for payload");
  }
  if (payloadLen < 18) {
    throw std::runtime_error("Serializer::decode: operation payload too short");
  }

  Operation op;
  uint8_t typeByte = data[offset++];
  op.type = (typeByte == 0x00) ? OpType::INSERT : OpType::DELETE;

  op.id.clock = readInt32(data, offset);
  op.id.siteID = readInt32(data, offset);
  op.leftNeighborID.clock = readInt32(data, offset);
  op.leftNeighborID.siteID = readInt32(data, offset);
  op.value = static_cast<char>(data[offset++]);

  return op;
}

std::vector<uint8_t> Serializer::encodeState(const CRDTEngine &engine) {
  std::vector<uint8_t> payload;

  writeInt32(payload, static_cast<int32_t>(engine.siteID_));
  writeInt32(payload, static_cast<int32_t>(engine.clock_));

  // Count nodes (excluding the sentinel)
  uint32_t nodeCount = 0;
  for (const auto &node : engine.seq_) {
    if (node.id == SENTINEL_ID)
      continue;
    ++nodeCount;
  }
  writeUint32(payload, nodeCount);

  // Write nodes in sequence order (sentinel excluded)
  for (const auto &node : engine.seq_) {
    if (node.id == SENTINEL_ID)
      continue;

    writeInt32(payload, static_cast<int32_t>(node.id.clock));
    writeInt32(payload, static_cast<int32_t>(node.id.siteID));
    writeInt32(payload, static_cast<int32_t>(node.leftNeighborID.clock));
    writeInt32(payload, static_cast<int32_t>(node.leftNeighborID.siteID));
    payload.push_back(static_cast<uint8_t>(node.value));
    payload.push_back(node.tombstoned ? 0x01 : 0x00);
  }

  return buildFrame(MsgType::STATE, payload);
}

CRDTEngine Serializer::decodeState(const std::vector<uint8_t> &bytes) {
  if (bytes.size() < 5) {
    throw std::runtime_error(
        "Serializer::decodeState: buffer too short for header");
  }

  const uint8_t *data = bytes.data();
  size_t offset = 0;

  uint8_t msgTypeByte = data[offset++];
  if (static_cast<MsgType>(msgTypeByte) != MsgType::STATE) {
    throw std::runtime_error(
        "Serializer::decodeState: expected STATE message type");
  }

  uint32_t payloadLen = readUint32(data, offset);
  if (bytes.size() - offset < payloadLen) {
    throw std::runtime_error(
        "Serializer::decodeState: buffer too short for payload");
  }
  if (payloadLen < 12) {
    throw std::runtime_error(
        "Serializer::decodeState: state payload too short");
  }

  int32_t siteID = readInt32(data, offset);
  int32_t clock = readInt32(data, offset);
  uint32_t nodeCount = readUint32(data, offset);

  size_t expectedPayload = 12 + static_cast<size_t>(nodeCount) * 18;
  if (payloadLen < expectedPayload) {
    throw std::runtime_error(
        "Serializer::decodeState: payload too short for node count");
  }

  // Reconstruct engine: build seq_ and index_ directly
  CRDTEngine engine(static_cast<int>(siteID));
  engine.clock_ = static_cast<int>(clock);

  // seq_ already contains the sentinel; rebuild the rest in stored order
  for (uint32_t i = 0; i < nodeCount; ++i) {
    CRDTEngine::Node node;
    node.id.clock = readInt32(data, offset);
    node.id.siteID = readInt32(data, offset);
    node.leftNeighborID.clock = readInt32(data, offset);
    node.leftNeighborID.siteID = readInt32(data, offset);
    node.value = static_cast<char>(data[offset++]);
    node.tombstoned = (data[offset++] != 0x00);

    engine.seq_.push_back(node);
    auto it = engine.seq_.end();
    --it;
    engine.index_[node.id] = it;
  }

  return engine;
}
