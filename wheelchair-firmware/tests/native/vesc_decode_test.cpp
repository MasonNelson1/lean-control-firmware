// Host test for src/vesc_decode.h (no hardware). Frames are built the way VESC
// firmware 6.06 builds them (buffer.c: float16/float32 = (int)(value * scale),
// big-endian), so a layout or scale mistake on our side fails here.
#include "vesc_decode.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
struct Buf
{
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void i16(int16_t v) { b.push_back((uint16_t)v >> 8); b.push_back(v & 0xFF); }
  void i32(int32_t v)
  {
    for (int s = 24; s >= 0; s -= 8)
      b.push_back(((uint32_t)v >> s) & 0xFF);
  }
  void f16(float v, float scale) { i16((int16_t)(v * scale)); }
  void f32(float v, float scale) { i32((int32_t)(v * scale)); }
};

bool near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }

// comm_can_send_status1..5 (comm_can.c, 6.06).
Buf status1(int32_t erpm, float current, float duty)
{
  Buf b;
  b.i32(erpm);
  b.i16((int16_t)(current * 1e1f));
  b.i16((int16_t)(duty * 1e3f));
  return b;
}

// commands.c COMM_GET_VALUES with mask 0xFFFFFFFF.
Buf valuesReply(uint8_t fault, bool full = true)
{
  Buf b;
  b.u8(VESC_COMM_GET_VALUES);
  b.f16(31.4f, 1e1f);      // temp_fet
  b.f16(27.5f, 1e1f);      // temp_motor
  b.f32(-41.23f, 1e2f);    // avg motor current
  b.f32(-3.21f, 1e2f);     // avg input current
  b.f32(0.12f, 1e2f);      // avg id
  b.f32(-41.0f, 1e2f);     // avg iq
  b.f16(-0.456f, 1e3f);    // duty
  b.f32(-8123.0f, 1e0f);   // rpm (erpm)
  b.f16(52.3f, 1e1f);      // v_in
  b.f32(0.1234f, 1e4f);    // Ah
  b.f32(0.0567f, 1e4f);    // Ah charged
  b.f32(6.4321f, 1e4f);    // Wh
  b.f32(2.5f, 1e4f);       // Wh charged
  b.i32(-123456);          // tachometer
  b.i32(654321);           // tachometer abs
  b.u8(fault);             // fault code
  if (!full)
    return b;
  b.f32(12.345678f, 1e6f); // pid pos
  b.u8(2);                 // controller id
  b.f16(30.1f, 1e1f);      // MOSFET 1..3 temps
  b.f16(30.2f, 1e1f);
  b.f16(30.3f, 1e1f);
  b.f32(1.234f, 1e3f);     // avg vd
  b.f32(-20.5f, 1e3f);     // avg vq
  b.u8(2);                 // status: kill switch active
  return b;
}
} // namespace

int main()
{
  // ---- STATUS_1..5
  VescStatus v{};
  Buf s1 = status1(-12345, -23.4f, 0.123f);
  assert(vescDecodeStatus(VESC_STATUS, s1.b.data(), 8, v, 1000));
  assert(v.erpm == -12345 && near(v.current, -23.4f) && near(v.duty, 0.123f));
  assert(v.rpmFrames == 1 && v.lastRpmMs == 1000 && v.frames == 1 && v.lastMs == 1000);
  assert(v.typeFrames[0] == 1 && v.typeMs[0] == 1000);

  Buf s2;
  s2.i32((int32_t)(1.2345f * 1e4f));
  s2.i32((int32_t)(0.0101f * 1e4f));
  assert(vescDecodeStatus(VESC_STATUS_2, s2.b.data(), 8, v, 1001));
  assert(near(v.ampHours, 1.2345f) && near(v.ampHoursCharged, 0.0101f) && v.typeFrames[1] == 1);

  Buf s3;
  s3.i32((int32_t)(65.4321f * 1e4f));
  s3.i32((int32_t)(1.5f * 1e4f));
  assert(vescDecodeStatus(VESC_STATUS_3, s3.b.data(), 8, v, 1002));
  assert(near(v.wattHours, 65.4321f, 1e-3f) && near(v.wattHoursCharged, 1.5f) && v.typeFrames[2] == 1);

  Buf s4;
  s4.i16((int16_t)(35.6f * 1e1f));
  s4.i16((int16_t)(-270.0f * 1e1f)); // no motor thermistor fitted
  s4.i16((int16_t)(-4.5f * 1e1f));
  s4.i16((int16_t)(90.0f * 50.0f));
  assert(vescDecodeStatus(VESC_STATUS_4, s4.b.data(), 8, v, 1003));
  assert(near(v.mosfetTempC, 35.6f) && near(v.motorTempC, -270.0f) && near(v.inputCurrent, -4.5f) &&
         near(v.pidPos, 90.0f) && v.typeFrames[3] == 1);

  Buf s5;
  s5.i32(-987654);
  s5.i16((int16_t)(52.7f * 1e1f));
  s5.i16(0);
  assert(vescDecodeStatus(VESC_STATUS_5, s5.b.data(), 8, v, 1004));
  assert(v.tachometer == -987654 && near(v.inputVoltage, 52.7f) && v.typeFrames[4] == 1);
  assert(v.frames == 5 && v.lastMs == 1004 && v.rpmFrames == 1);

  // Short frames and other types leave the status untouched.
  const VescStatus before = v;
  assert(!vescDecodeStatus(VESC_STATUS, s1.b.data(), 7, v, 2000));
  assert(!vescDecodeStatus(VESC_STATUS_4, s4.b.data(), 7, v, 2000));
  assert(!vescDecodeStatus(VESC_STATUS_5, s5.b.data(), 5, v, 2000));
  assert(!vescDecodeStatus(58, s1.b.data(), 8, v, 2000)); // STATUS_6 (ADC/PPM): not decoded
  assert(!vescDecodeStatus(3, s1.b.data(), 8, v, 2000));  // SET_RPM echoed by another node
  assert(v.frames == before.frames && v.lastMs == before.lastMs && v.erpm == before.erpm &&
         v.mosfetTempC == before.mosfetTempC && v.tachometer == before.tachometer &&
         memcmp(v.typeFrames, before.typeFrames, sizeof v.typeFrames) == 0);

  // Peaks keep the largest magnitude, signed, until cleared.
  VescStatus p{};
  for (float amps : {5.0f, -30.0f, 12.0f})
  {
    Buf f = status1(0, amps, 0);
    vescDecodeStatus(VESC_STATUS, f.b.data(), 8, p, 0);
  }
  assert(near(p.currentPeak, -30.0f) && near(p.current, 12.0f));
  vescClearPeaks(p);
  assert(near(p.currentPeak, 12.0f));

  // ---- COMM_GET_VALUES
  Buf full = valuesReply(4);
  assert(full.b.size() == VESC_VALUES_FULL_LEN);
  VescValues val{};
  assert(vescDecodeValues(full.b.data(), full.b.size(), val));
  assert(val.full);
  assert(near(val.tempFet, 31.4f) && near(val.tempMotor, 27.5f));
  assert(near(val.currentMotor, -41.23f) && near(val.currentIn, -3.21f));
  assert(near(val.currentD, 0.12f) && near(val.currentQ, -41.0f));
  assert(near(val.duty, -0.456f) && near(val.erpm, -8123.0f) && near(val.inputVoltage, 52.3f));
  assert(near(val.ampHours, 0.1234f) && near(val.ampHoursCharged, 0.0567f));
  assert(near(val.wattHours, 6.4321f, 1e-3f) && near(val.wattHoursCharged, 2.5f));
  assert(val.tachometer == -123456 && val.tachometerAbs == 654321);
  assert(val.fault == 4 && strcmp(vescFaultName(val.fault), "ABS_OVER_CURRENT") == 0);
  assert(near(val.pidPos, 12.345678f, 1e-5f) && val.controllerId == 2);
  assert(near(val.tempMos[0], 30.1f) && near(val.tempMos[1], 30.2f) && near(val.tempMos[2], 30.3f));
  assert(near(val.voltageD, 1.234f) && near(val.voltageQ, -20.5f) && val.status == 2);

  // A reply cut after the fault code (older firmware) still yields the fault.
  Buf partial = valuesReply(1, false);
  assert(partial.b.size() == VESC_VALUES_MIN_LEN);
  assert(vescDecodeValues(partial.b.data(), partial.b.size(), val));
  assert(!val.full && val.fault == 1 && val.tachometerAbs == 654321 && val.controllerId == 0);
  // Anything shorter, or another command, is rejected and leaves out untouched.
  val.fault = 99;
  assert(!vescDecodeValues(partial.b.data(), partial.b.size() - 1, val));
  Buf other = valuesReply(0);
  other.b[0] = 14; // COMM_GET_MCCONF
  assert(!vescDecodeValues(other.b.data(), other.b.size(), val));
  assert(val.fault == 99);

  // Fault names follow mc_fault_code (datatypes.h, 6.06).
  assert(strcmp(vescFaultName(0), "NONE") == 0);
  assert(strcmp(vescFaultName(1), "OVER_VOLTAGE") == 0);
  assert(strcmp(vescFaultName(3), "DRV") == 0);
  assert(strcmp(vescFaultName(5), "OVER_TEMP_FET") == 0);
  assert(strcmp(vescFaultName(10), "BOOTING_FROM_WATCHDOG_RESET") == 0);
  assert(strcmp(vescFaultName(18), "UNBALANCED_CURRENTS") == 0);
  assert(strcmp(vescFaultName(29), "LV_OUTPUT_FAULT") == 0);
  assert(strcmp(vescFaultName(30), "UNKNOWN") == 0);
  assert(strcmp(vescFaultName(255), "UNKNOWN") == 0);

  printf("vesc decode: status 1-5, GET_VALUES (full and short), fault names\n");
  return 0;
}
