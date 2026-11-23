#pragma once

#include <math.h>
#include <stdint.h>

// VESC CAN protocol, independent of the CAN driver so the host tests can run it.
//   - extended 29-bit ids, 500 kbps
//   - id = controller_id | (packet_type << 8)
//   - multi-byte values big-endian, signed

// Status frame layouts, VescStatus and their decoding live in vesc_decode.h
// (charles/dev-setup, every STATUS_1..5 field); this file keeps the command
// side and the helpers council/firmware-9's drive code uses.
#include "vesc_decode.h"

// Command packet types
const uint8_t VESC_SET_DUTY = 0;          // int32 duty * 100000
const uint8_t VESC_SET_CURRENT = 1;       // int32 amps * 1000
const uint8_t VESC_SET_CURRENT_BRAKE = 2; // int32 amps * 1000; brakes toward zero speed, never reverses
const uint8_t VESC_SET_ERPM = 3;          // int32 erpm, unscaled.
// 14 is the gear and belt reduction and 8 is because its an 8 pole motor and for some reason you need to do half that
//  and the vesc takes erpm so you have to account for that.
constexpr float VESC_ERPM_PER_WHEEL_RPM = 14.5454f * 4.0f;

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
  if (!extended || (canId & 0xFF) != controllerId)
    return;
  vescDecodeStatus((canId >> 8) & 0xFF, data, length, v, nowMs);
}

// True if this VESC has reported its speed within timeoutMs. Only the speed
// frame counts: it is the one the drive loop depends on.
inline bool vescFresh(const VescStatus &v, uint32_t nowMs, uint32_t timeoutMs)
{
  return v.rpmFrames > 0 && nowMs - v.lastRpmMs <= timeoutMs;
}
