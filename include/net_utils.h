#pragma once

#include <cstdint>
#include <string>

// Parse "ip:port" into ip and port components.
// If no colon is found, ip = addr and port = defaultPort.
inline void parseAddr(const std::string &addr, std::string &ip, uint16_t &port,
                      uint16_t defaultPort = 0) {
  auto colon = addr.rfind(':');
  if (colon != std::string::npos) {
    ip = addr.substr(0, colon);
    port = static_cast<uint16_t>(std::stoi(addr.substr(colon + 1)));
  } else {
    ip = addr;
    port = defaultPort;
  }
}

// All four UDP/TCP ports used by a single peer instance, derived from its
// base data port. Keeping them in one place prevents off-by-one errors in
// the callbacks that reconstruct peer port numbers from received addresses.
//
//   data   — UDP CRDT operation socket
//   hb     — UDP heartbeat / peer-discovery socket  (data + 1)
//   tcp    — TCP state-sync server port             (data + 2)
//   cursor — UDP remote cursor position socket      (data + 3)
struct PortLayout {
  uint16_t data;
  uint16_t hb;
  uint16_t tcp;
  uint16_t cursor;

  static PortLayout fromData(uint16_t dataPort) {
    return {dataPort, static_cast<uint16_t>(dataPort + 1),
            static_cast<uint16_t>(dataPort + 2),
            static_cast<uint16_t>(dataPort + 3)};
  }
};
