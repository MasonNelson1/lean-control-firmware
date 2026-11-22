#pragma once
#include "vesc_can.h"

// ============================================================
// Read-only VESC query over CAN (2026-11-22 test).
// Asks each VESC for its firmware version, the same way VESC Tool reaches a
// second VESC through "CAN forward": a COMM packet wrapped in CAN buffer
// frames. If replies come back, the Teensy can act as a VESC Tool bridge.
// Only GET commands are ever sent (firmware version, motor config, app config):
// they change nothing and move nothing. The raw config bytes are streamed over
// Bluetooth and decoded on the laptop by tools/vesc_conf.py.
// ============================================================

namespace vescprobe
{
// Our CAN id for replies. Must not match a VESC (2, 26) or 255 (broadcast).
constexpr uint8_t PROBE_CAN_ID = 77;

constexpr uint8_t CAN_FILL_RX_BUFFER = 5;
constexpr uint8_t CAN_FILL_RX_BUFFER_LONG = 6;
constexpr uint8_t CAN_PROCESS_RX_BUFFER = 7;
constexpr uint8_t CAN_PROCESS_SHORT_BUFFER = 8;
constexpr uint8_t COMM_FW_VERSION = 0;
constexpr uint8_t COMM_GET_MCCONF = 14;
constexpr uint8_t COMM_GET_APPCONF = 17;

struct Reply
{
  bool done = false;
  uint8_t fromId = 0;
  uint16_t len = 0;
  bool crcOk = false;
  uint8_t data[1024]; // mcconf is 484 bytes on firmware 6.06
};

inline uint16_t crc16(const uint8_t *p, uint16_t n)
{
  uint16_t c = 0;
  for (uint16_t i = 0; i < n; ++i)
  {
    c ^= (uint16_t)p[i] << 8;
    for (int b = 0; b < 8; ++b)
      c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

// Ask one VESC a one-byte GET command (fits a short-buffer frame).
inline void ask(VescBus &bus, uint8_t vescId, uint8_t comm)
{
  CAN_message_t m;
  m.id = vescId | ((uint32_t)CAN_PROCESS_SHORT_BUFFER << 8);
  m.flags.extended = 1;
  m.len = 3;
  m.buf[0] = PROBE_CAN_ID; // reply to us
  m.buf[1] = 0;            // 0 = process the command and send the answer back
  m.buf[2] = comm;
  bus.write(m);
}

// Collect reply frames addressed to PROBE_CAN_ID. Returns true when a full
// reply is ready in r (caller clears r.done after using it).
inline bool handleFrame(const CAN_message_t &msg, Reply &r)
{
  if (!msg.flags.extended || (msg.id & 0xFF) != PROBE_CAN_ID)
    return false;
  const uint8_t type = (msg.id >> 8) & 0xFF;
  if (type == CAN_FILL_RX_BUFFER && msg.len >= 2)
  {
    const uint16_t off = msg.buf[0];
    for (uint8_t i = 1; i < msg.len && off + i - 1 < (int)sizeof(r.data); ++i)
      r.data[off + i - 1] = msg.buf[i];
  }
  else if (type == CAN_FILL_RX_BUFFER_LONG && msg.len >= 3)
  {
    const uint16_t off = ((uint16_t)msg.buf[0] << 8) | msg.buf[1];
    for (uint8_t i = 2; i < msg.len && off + i - 2 < (int)sizeof(r.data); ++i)
      r.data[off + i - 2] = msg.buf[i];
  }
  else if (type == CAN_PROCESS_RX_BUFFER && msg.len >= 6)
  {
    r.fromId = msg.buf[0];
    r.len = ((uint16_t)msg.buf[2] << 8) | msg.buf[3];
    const uint16_t crc = ((uint16_t)msg.buf[4] << 8) | msg.buf[5];
    r.crcOk = r.len <= sizeof(r.data) && crc16(r.data, r.len) == crc;
    r.done = true;
    return true;
  }
  else if (type == CAN_PROCESS_SHORT_BUFFER && msg.len >= 2)
  {
    r.fromId = msg.buf[0];
    r.len = msg.len - 2;
    for (uint8_t i = 0; i < r.len; ++i)
      r.data[i] = msg.buf[2 + i];
    r.crcOk = true;
    r.done = true;
    return true;
  }
  return false;
}
} // namespace vescprobe
