#pragma once

#include <math.h>
#include <stdint.h>

// VESC CAN protocol, independent of the CAN driver so the host tests can run it.
//   - extended 29-bit ids, 500 kbps
//   - id = controller_id | (packet_type << 8)
//   - multi-byte values big-endian, signed

// Packet types
const uint8_t VESC_SET_DUTY = 0;          // int32 duty * 100000
const uint8_t VESC_SET_CURRENT = 1;       // int32 amps * 1000
const uint8_t VESC_SET_CURRENT_BRAKE = 2; // int32 amps * 1000; brakes toward zero speed, never reverses
const uint8_t VESC_SET_ERPM = 3;          // int32 erpm, unscaled.
const uint8_t VESC_STATUS = 9;            // ERPM, current, duty
const uint8_t VESC_STATUS_4 = 16;         // MOSFET temp, motor temp, input current
const uint8_t VESC_STATUS_5 = 27;         // tachometer, input voltage
// 14 is the gear and belt reduction and 8 is because its an 8 pole motor and for some reason you need to do half that
//  and the vesc takes erpm so you have to account for that.
constexpr float VESC_ERPM_PER_WHEEL_RPM = 14.5454f * 4.0f;

// Latest status reported by one VESC.
struct VescStatus
{
  uint8_t id; // controller id (set this, the rest starts at 0)
  int32_t erpm;
  float current; // motor current, A
  float duty;    // duty the VESC is running
  float mosfetTempC;
  float inputVoltage;
  uint32_t frames; // status frames received
  uint32_t rpmFrames;
  uint32_t lastRpmMs;
  uint32_t lastMs; // millis() of the last one
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

// Wheel RPM -> the ERPM the VESC takes. Fractions of a wheel RPM survive: one
// wheel RPM is 58 ERPM, so rounding to whole wheel RPM first threw away steps.
inline int32_t vescWheelRpmToErpm(float wheelRpm)
{
  return (int32_t)lroundf(wheelRpm * VESC_ERPM_PER_WHEEL_RPM);
}
inline float vescWheelRpm(const VescStatus &v)
{
  return v.erpm / VESC_ERPM_PER_WHEEL_RPM;
}

// Decode one received frame into v. Anything that isn't a status frame from
// controllerId is ignored.
inline void vescDecode(uint32_t canId, bool extended, uint8_t length, const uint8_t *data,
                       uint8_t controllerId, VescStatus &v, uint32_t nowMs)
{
  if (!extended)
    return;
  const uint8_t ctrlId = canId & 0xFF;
  const uint8_t type = (canId >> 8) & 0xFF;
  if (controllerId != ctrlId)
    return;
  if (type == VESC_STATUS && length >= 8)
  {
    v.erpm = vescBe32(&data[0]);
    v.rpmFrames++;
    v.lastRpmMs = nowMs;
    v.current = vescBe16(&data[4]) / 10.0f;
    v.duty = vescBe16(&data[6]) / 1000.0f;
  }
  else if (type == VESC_STATUS_4 && length >= 2)
  {
    v.mosfetTempC = vescBe16(&data[0]) / 10.0f;
  }
  else if (type == VESC_STATUS_5 && length >= 6)
  {
    v.inputVoltage = vescBe16(&data[4]) / 10.0f;
  }
  else
  {
    return;
  }
  v.frames++;
  v.lastMs = nowMs;
}

// True if this VESC has reported its speed within timeoutMs. Only the speed
// frame counts: it is the one the drive loop depends on.
inline bool vescFresh(const VescStatus &v, uint32_t nowMs, uint32_t timeoutMs)
{
  return v.rpmFrames > 0 && nowMs - v.lastRpmMs <= timeoutMs;
}
