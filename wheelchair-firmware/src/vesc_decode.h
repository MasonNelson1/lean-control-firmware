#pragma once
#include <stdint.h>
#include <stddef.h>

// ============================================================
// VESC decoders with no hardware dependency (unit-tested on the host in
// tests/native/vesc_decode_test.cpp). Layouts are from VESC firmware 6.06
// (vedderb/bldc release_6_06): comm/comm_can.c comm_can_send_status1..5 and
// comm/commands.c COMM_GET_VALUES. Big-endian, signed, fixed-point.
// ============================================================

// CAN status packet types (comm_can.c / datatypes.h CAN_PACKET_ID).
const uint8_t VESC_STATUS = 9;    // STATUS_1: int32 erpm, int16 current*10, int16 duty*1000
const uint8_t VESC_STATUS_2 = 14; // int32 Ah used*1e4, int32 Ah charged*1e4
const uint8_t VESC_STATUS_3 = 15; // int32 Wh used*1e4, int32 Wh charged*1e4
const uint8_t VESC_STATUS_4 = 16; // int16 MOSFET temp*10, motor temp*10, input current*10, PID pos*50
const uint8_t VESC_STATUS_5 = 27; // int32 tachometer, int16 input voltage*10, int16 reserved

// Latest status reported by one VESC over the CAN status frames.
struct VescStatus
{
  uint8_t id; // controller id (set this, the rest starts at 0)
  // STATUS_1 (50 Hz)
  int32_t erpm;
  float current; // motor current (filtered), A
  float duty;    // duty the VESC is running
  // STATUS_2 / STATUS_3 (50 Hz): counters since the VESC booted
  float ampHours, ampHoursCharged;
  float wattHours, wattHoursCharged;
  // STATUS_4 (50 Hz)
  float mosfetTempC;
  float motorTempC;   // reads ≈ -270 or garbage with no motor thermistor fitted
  float inputCurrent; // battery-side current (filtered), A
  float pidPos;       // position PID, degrees (unused by this chair)
  // STATUS_5 (5 Hz once can_status_msgs_r2 enables it)
  int32_t tachometer; // electrical steps; 6 per electrical revolution
  float inputVoltage;
  // Peaks since the last clearPeaks(): the signed value with the largest
  // magnitude, so a 1 Hz log still shows a 20 ms spike.
  float currentPeak, inputCurrentPeak;
  uint32_t frames; // status frames received (any type)
  uint32_t rpmFrames;
  uint32_t lastRpmMs;
  uint32_t lastMs; // millis() of the last one
  // Per type, index 0..4 = STATUS_1..STATUS_5: frames received and millis() of the last.
  uint32_t typeFrames[5];
  uint32_t typeMs[5];
};

inline int32_t vescBe32(const uint8_t *p)
{
  return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}
inline int16_t vescBe16(const uint8_t *p)
{
  return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

// Index 0..4 for STATUS_1..STATUS_5, or -1 for anything else.
inline int vescStatusIndex(uint8_t type)
{
  switch (type)
  {
  case VESC_STATUS: return 0;
  case VESC_STATUS_2: return 1;
  case VESC_STATUS_3: return 2;
  case VESC_STATUS_4: return 3;
  case VESC_STATUS_5: return 4;
  default: return -1;
  }
}

inline void vescKeepPeak(float &peak, float value)
{
  if ((value < 0 ? -value : value) > (peak < 0 ? -peak : peak))
    peak = value;
}

// Decode one status frame's payload into v. Returns false (v untouched) for
// other packet types or short frames.
inline bool vescDecodeStatus(uint8_t type, const uint8_t *b, uint8_t len, VescStatus &v, uint32_t now)
{
  const int index = vescStatusIndex(type);
  if (index < 0)
    return false;
  switch (type)
  {
  case VESC_STATUS:
    if (len < 8)
      return false;
    v.erpm = vescBe32(&b[0]);
    v.current = vescBe16(&b[4]) / 10.0f;
    v.duty = vescBe16(&b[6]) / 1000.0f;
    vescKeepPeak(v.currentPeak, v.current);
    v.rpmFrames++;
    v.lastRpmMs = now;
    break;
  case VESC_STATUS_2:
    if (len < 8)
      return false;
    v.ampHours = vescBe32(&b[0]) / 1e4f;
    v.ampHoursCharged = vescBe32(&b[4]) / 1e4f;
    break;
  case VESC_STATUS_3:
    if (len < 8)
      return false;
    v.wattHours = vescBe32(&b[0]) / 1e4f;
    v.wattHoursCharged = vescBe32(&b[4]) / 1e4f;
    break;
  case VESC_STATUS_4:
    if (len < 8)
      return false;
    v.mosfetTempC = vescBe16(&b[0]) / 10.0f;
    v.motorTempC = vescBe16(&b[2]) / 10.0f;
    v.inputCurrent = vescBe16(&b[4]) / 10.0f;
    v.pidPos = vescBe16(&b[6]) / 50.0f;
    vescKeepPeak(v.inputCurrentPeak, v.inputCurrent);
    break;
  case VESC_STATUS_5:
    if (len < 6)
      return false;
    v.tachometer = vescBe32(&b[0]);
    v.inputVoltage = vescBe16(&b[4]) / 10.0f;
    break;
  }
  v.typeFrames[index]++;
  v.typeMs[index] = now;
  v.frames++;
  v.lastMs = now;
  return true;
}

inline void vescClearPeaks(VescStatus &v)
{
  v.currentPeak = v.current;
  v.inputCurrentPeak = v.inputCurrent;
}

// ------------------------------------------------------------
// COMM_GET_VALUES reply (commands.c, 6.06). Field order with mask 0xFFFFFFFF:
//   temp_fet f16/10, temp_motor f16/10, avg motor current f32/100,
//   avg input current f32/100, avg id f32/100, avg iq f32/100, duty f16/1000,
//   rpm (erpm) f32/1, v_in f16/10, Ah f32/1e4, Ah charged f32/1e4, Wh f32/1e4,
//   Wh charged f32/1e4, tachometer i32, tachometer abs i32, fault u8,
//   pid pos f32/1e6, controller id u8, MOSFET 1-3 temps 3×f16/10,
//   avg vd f32/1e3, avg vq f32/1e3, status u8 (bit0 timeout, bit1 kill switch).
// The averages reset on every read, so each is the mean since our last poll.
// ------------------------------------------------------------
const uint8_t VESC_COMM_GET_VALUES = 4;
const uint16_t VESC_VALUES_MIN_LEN = 54; // command byte through the fault code
const uint16_t VESC_VALUES_FULL_LEN = 74;

struct VescValues
{
  float tempFet, tempMotor;
  float currentMotor, currentIn, currentD, currentQ;
  float duty, erpm, inputVoltage;
  float ampHours, ampHoursCharged, wattHours, wattHoursCharged;
  int32_t tachometer, tachometerAbs;
  uint8_t fault;
  // Present only in a full-length (74 byte) reply; `full` says which.
  bool full;
  float pidPos;
  uint8_t controllerId;
  float tempMos[3];
  float voltageD, voltageQ;
  uint8_t status;
};

// p[0] must be COMM_GET_VALUES. Returns false for anything shorter than the
// fault code; fields after it are filled only when the whole reply is there.
inline bool vescDecodeValues(const uint8_t *p, uint16_t len, VescValues &out)
{
  if (len < VESC_VALUES_MIN_LEN || p[0] != VESC_COMM_GET_VALUES)
    return false;
  VescValues v{};
  uint16_t i = 1;
  auto f16 = [&](float scale) { const float x = vescBe16(p + i) / scale; i += 2; return x; };
  auto f32 = [&](float scale) { const float x = vescBe32(p + i) / scale; i += 4; return x; };
  auto i32 = [&]() { const int32_t x = vescBe32(p + i); i += 4; return x; };
  v.tempFet = f16(10);
  v.tempMotor = f16(10);
  v.currentMotor = f32(100);
  v.currentIn = f32(100);
  v.currentD = f32(100);
  v.currentQ = f32(100);
  v.duty = f16(1000);
  v.erpm = f32(1);
  v.inputVoltage = f16(10);
  v.ampHours = f32(1e4f);
  v.ampHoursCharged = f32(1e4f);
  v.wattHours = f32(1e4f);
  v.wattHoursCharged = f32(1e4f);
  v.tachometer = i32();
  v.tachometerAbs = i32();
  v.fault = p[i++];
  if (len >= VESC_VALUES_FULL_LEN)
  {
    v.full = true;
    v.pidPos = f32(1e6f);
    v.controllerId = p[i++];
    for (float &t : v.tempMos)
      t = f16(10);
    v.voltageD = f32(1e3f);
    v.voltageQ = f32(1e3f);
    v.status = p[i++];
  }
  out = v;
  return true;
}

// mc_fault_code (datatypes.h, 6.06) without the FAULT_CODE_ prefix.
inline const char *vescFaultName(uint8_t code)
{
  static const char *const names[] = {
      "NONE", "OVER_VOLTAGE", "UNDER_VOLTAGE", "DRV", "ABS_OVER_CURRENT",
      "OVER_TEMP_FET", "OVER_TEMP_MOTOR", "GATE_DRIVER_OVER_VOLTAGE",
      "GATE_DRIVER_UNDER_VOLTAGE", "MCU_UNDER_VOLTAGE", "BOOTING_FROM_WATCHDOG_RESET",
      "ENCODER_SPI", "ENCODER_SINCOS_BELOW_MIN_AMPLITUDE", "ENCODER_SINCOS_ABOVE_MAX_AMPLITUDE",
      "FLASH_CORRUPTION", "HIGH_OFFSET_CURRENT_SENSOR_1", "HIGH_OFFSET_CURRENT_SENSOR_2",
      "HIGH_OFFSET_CURRENT_SENSOR_3", "UNBALANCED_CURRENTS", "BRK", "RESOLVER_LOT",
      "RESOLVER_DOS", "RESOLVER_LOS", "FLASH_CORRUPTION_APP_CFG", "FLASH_CORRUPTION_MC_CFG",
      "ENCODER_NO_MAGNET", "ENCODER_MAGNET_TOO_STRONG", "PHASE_FILTER", "ENCODER_FAULT",
      "LV_OUTPUT_FAULT"};
  return code < sizeof(names) / sizeof(names[0]) ? names[code] : "UNKNOWN";
}
