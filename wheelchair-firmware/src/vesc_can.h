#pragma once
#include <Arduino.h>
#include <FlexCAN_T4.h>

// ============================================================
// VESC CAN protocol helpers (see CLAUDE.md "VESC CAN protocol")
//   - extended 29-bit ids, 500 kbps
//   - id = controller_id | (packet_type << 8)
//   - multi-byte values big-endian, signed
// ============================================================

typedef FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> VescBus;   // CAN1: TX 22, RX 23

const uint32_t VESC_CAN_BAUD = 500000;

// Packet types
const uint8_t VESC_SET_DUTY    = 0;    // int32 duty * 100000
const uint8_t VESC_SET_ERPM    = 3;    //int32 erpm, unscaled. 
const uint8_t VESC_SET_CURRENT = 1;    // int32 amps * 1000
const uint8_t VESC_STATUS      = 9;    // ERPM, current, duty
const uint8_t VESC_STATUS_4    = 16;   // MOSFET temp, motor temp, input current
const uint8_t VESC_STATUS_5    = 27;   // tachometer, input voltage

// Latest status reported by one VESC.
struct VescStatus {
  uint8_t  id;            // controller id (set this, the rest starts at 0)
  int32_t  erpm;
  float    current;       // motor current, A
  float    duty;          // duty the VESC is running
  float    mosfetTempC;
  float    inputVoltage;
  uint32_t frames;        // status frames received
  uint32_t lastMs;        // millis() of the last one
};

inline int32_t vescBe32(const uint8_t *p) {
  return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8)  |  (uint32_t)p[3]);
}
inline int16_t vescBe16(const uint8_t *p) {
  return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

inline void vescBegin(VescBus &bus) {
  bus.begin();
  bus.setBaudRate(VESC_CAN_BAUD);
  bus.setMaxMB(16);
  bus.enableFIFO();
  bus.setFIFOFilter(ACCEPT_ALL);
}

// One command frame: int32 payload, big-endian.
inline void vescSend(VescBus &bus, uint8_t id, uint8_t type, int32_t value) {
  CAN_message_t m;
  m.id = id | ((uint32_t)type << 8);
  m.flags.extended = 1;
  m.len = 4;
  m.buf[0] = (value >> 24) & 0xFF;
  m.buf[1] = (value >> 16) & 0xFF;
  m.buf[2] = (value >> 8)  & 0xFF;
  m.buf[3] =  value        & 0xFF;
  bus.write(m);
}

inline void vescSetDuty(VescBus &bus, uint8_t id, float duty) {
  vescSend(bus, id, VESC_SET_DUTY, (int32_t)lroundf(duty * 100000.0f));
}

inline void vescSetWheelRPM(VescBus &bus, uint8_t id, int32_t rpm) {
   //14 is the gear and belt reduction and 8 is because its an 8 pole motor and for some reason you need to do half that 
   // and the vesc takes erpm so you have to account for that. 
  vescSend(bus, id, VESC_SET_ERPM, (int32_t)lroundf(rpm * 14.5454f * 4.0f));
}

inline void vescSetCurrent(VescBus &bus, uint8_t id, float amps) {
  vescSend(bus, id, VESC_SET_CURRENT, (int32_t)lroundf(amps * 1000.0f));
}

// Decode a received frame into the matching entry of vescs[]. Anything that
// isn't a status frame from one of those ids is ignored.
inline void vescHandleFrame(const CAN_message_t &msg, VescStatus *vescs, int n) {
  if (!msg.flags.extended) return;
  uint8_t ctrlId = msg.id & 0xFF;
  uint8_t type   = (msg.id >> 8) & 0xFF;
  for (int i = 0; i < n; i++) {
    VescStatus &v = vescs[i];
    if (v.id != ctrlId) continue;
    if (type == VESC_STATUS && msg.len >= 8) {
      v.erpm    = vescBe32(&msg.buf[0]);
      v.current = vescBe16(&msg.buf[4]) / 10.0f;
      v.duty    = vescBe16(&msg.buf[6]) / 1000.0f;
    } else if (type == VESC_STATUS_4 && msg.len >= 2) {
      v.mosfetTempC = vescBe16(&msg.buf[0]) / 10.0f;
    } else if (type == VESC_STATUS_5 && msg.len >= 6) {
      v.inputVoltage = vescBe16(&msg.buf[4]) / 10.0f;
    } else {
      return;
    }
    v.frames++;
    v.lastMs = millis();
    return;
  }
}

// True if this VESC has reported status within timeoutMs.
inline bool vescFresh(const VescStatus &v, uint32_t timeoutMs) {
  return v.frames > 0 && millis() - v.lastMs <= timeoutMs;
}
