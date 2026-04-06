#pragma once

#include "rga.h"
#include <cstdint>
#include <vector>

// Top-level message type byte
enum class MsgType : uint8_t { OPERATION = 0x01, STATE = 0x02 };

// Wire format for all messages:
//   [1 byte : MsgType]
//   [4 bytes: payload length, big-endian uint32]
//   [N bytes: payload]
//
// Operation payload (18 bytes):
//   [1 byte : OpType  (0 = INSERT, 1 = DELETE)]
//   [4 bytes: id.clock           (big-endian int32)]
//   [4 bytes: id.siteID          (big-endian int32)]
//   [4 bytes: leftNeighborID.clock  (big-endian int32)]
//   [4 bytes: leftNeighborID.siteID (big-endian int32)]
//   [1 byte : value (char)]
//
// State payload:
//   [4 bytes: siteID             (big-endian int32)]
//   [4 bytes: clock              (big-endian int32)]
//   [4 bytes: node count         (big-endian uint32)]
//   For each node (18 bytes):
//     [4 bytes: id.clock           (big-endian int32)]
//     [4 bytes: id.siteID          (big-endian int32)]
//     [4 bytes: leftNeighborID.clock  (big-endian int32)]
//     [4 bytes: leftNeighborID.siteID (big-endian int32)]
//     [1 byte : value (char)]
//     [1 byte : tombstoned (0 or 1)]

class Serializer {
public:
  // Encode a single Operation → framed byte buffer
  static std::vector<uint8_t> encode(const Operation &op);

  // Decode a framed byte buffer → Operation
  // Throws std::runtime_error on malformed input
  static Operation decode(const std::vector<uint8_t> &bytes);

  // Encode full CRDTEngine state → framed byte buffer
  static std::vector<uint8_t> encodeState(const CRDTEngine &engine);

  // Reconstruct a CRDTEngine from a framed byte buffer
  // Throws std::runtime_error on malformed input
  static CRDTEngine decodeState(const std::vector<uint8_t> &bytes);

  // Big-endian encode/decode helpers — also used by other modules.
  static void     writeUint16(std::vector<uint8_t> &buf, uint16_t val);
  static uint16_t readUint16(const uint8_t *data, size_t &offset);

  static void     writeUint32(std::vector<uint8_t> &buf, uint32_t val);
  static uint32_t readUint32(const uint8_t *data, size_t &offset);

  static void    writeInt32(std::vector<uint8_t> &buf, int32_t val);
  static int32_t readInt32(const uint8_t *data, size_t &offset);
};
