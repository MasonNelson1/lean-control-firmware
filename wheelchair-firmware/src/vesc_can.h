#pragma once
#include <Arduino.h>
#include <FlexCAN_T4.h>
#include "vesc_decode.h"

// ============================================================
// VESC CAN protocol helpers (see CLAUDE.md "VESC CAN protocol")
//   - extended 29-bit ids, 500 kbps
//   - id = controller_id | (packet_type << 8)
//   - multi-byte values big-endian, signed
// ============================================================

typedef FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> VescBus; // CAN1: TX 22, RX 23

const uint32_t VESC_CAN_BAUD = 500000;

// Command packet types. Status types and decoding are in vesc_decode.h.
const uint8_t VESC_SET_DUTY = 0;    // int32 duty * 100000
const uint8_t VESC_SET_ERPM = 3;    // int32 erpm, unscaled.
const uint8_t VESC_SET_CURRENT = 1; // int32 amps * 1000
constexpr float VESC_ERPM_PER_WHEEL_RPM = 14.5454f * 4.0f;

inline void vescBegin(VescBus &bus)
{
  bus.begin();
  bus.setBaudRate(VESC_CAN_BAUD);
  bus.setMaxMB(16);
  bus.enableFIFO();
  bus.setFIFOFilter(ACCEPT_ALL);
}

// Command frames the CAN driver refused (TX queue full), for DBG SYS.
inline uint32_t &vescTxFailures()
{
  static uint32_t failures = 0;
  return failures;
}

// One command frame: int32 payload, big-endian.
inline void vescSend(VescBus &bus, uint8_t id, uint8_t type, int32_t value)
{
  CAN_message_t m;
  m.id = id | ((uint32_t)type << 8);
  m.flags.extended = 1;
  m.len = 4;
  m.buf[0] = (value >> 24) & 0xFF;
  m.buf[1] = (value >> 16) & 0xFF;
  m.buf[2] = (value >> 8) & 0xFF;
  m.buf[3] = value & 0xFF;
  if (!bus.write(m))
    ++vescTxFailures(); // TX queue full: this command was dropped (counted for DBG SYS)
}

inline void vescSetDuty(VescBus &bus, uint8_t id, float duty)
{
  vescSend(bus, id, VESC_SET_DUTY, (int32_t)lroundf(duty * 100000.0f));
}

inline void vescSetWheelRPM(VescBus &bus, uint8_t id, int32_t rpm)
{
  // 14 is the gear and belt reduction and 8 is because its an 8 pole motor and for some reason you need to do half that
  //  and the vesc takes erpm so you have to account for that.
  vescSend(bus, id, VESC_SET_ERPM, (int32_t)lroundf(rpm * VESC_ERPM_PER_WHEEL_RPM));
}

inline void vescSetCurrent(VescBus &bus, uint8_t id, float amps)
{
  vescSend(bus, id, VESC_SET_CURRENT, (int32_t)lroundf(amps * 1000.0f));
}

// Decode a received frame into v if it is a status frame (STATUS_1..5) from
// controllerId. Anything else is ignored.
inline void vescHandleFrame(const CAN_message_t &msg, uint8_t controllerId, VescStatus &v)
{
  if (!msg.flags.extended || (msg.id & 0xFF) != controllerId)
    return;
  vescDecodeStatus((msg.id >> 8) & 0xFF, msg.buf, msg.len, v, millis());
}

// True if this VESC has reported status within timeoutMs.
inline bool vescFresh(const VescStatus &v, uint32_t timeoutMs)
{
  return v.frames > 0 && millis() - v.lastMs <= timeoutMs;
}
