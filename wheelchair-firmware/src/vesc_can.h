#pragma once
#include <Arduino.h>
#include <FlexCAN_T4.h>
#include "vesc_protocol.h" // includes vesc_decode.h

// CAN driver side of the VESC link. The protocol itself is in vesc_protocol.h.

typedef FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> VescBus; // CAN1: TX 22, RX 23

const uint32_t VESC_CAN_BAUD = 500000;

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

inline void vescSetWheelRPM(VescBus &bus, uint8_t id, float rpm)
{
  vescSend(bus, id, VESC_SET_ERPM, vescWheelRpmToErpm(rpm));
}

inline void vescSetCurrent(VescBus &bus, uint8_t id, float amps)
{
  vescSend(bus, id, VESC_SET_CURRENT, (int32_t)lroundf(amps * 1000.0f));
}

// Brake with this much current until the wheel stops. The VESC takes the
// magnitude: it always opposes the rotation and never drives in reverse.
inline void vescSetBrakeCurrent(VescBus &bus, uint8_t id, float amps)
{
  vescSend(bus, id, VESC_SET_CURRENT_BRAKE, (int32_t)lroundf(fabsf(amps) * 1000.0f));
}

inline void vescHandleFrame(const CAN_message_t &msg, uint8_t controllerId, VescStatus &v)
{
  vescDecode(msg.id, msg.flags.extended, msg.len, msg.buf, controllerId, v, millis());
}
