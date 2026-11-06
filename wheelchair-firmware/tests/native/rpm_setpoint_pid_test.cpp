// Host test (does not command hardware):
// g++ -std=c++11 -Wall -Wextra -Werror -I tests/native/arduino_stub -I src tests/native/rpm_setpoint_pid_test.cpp -o /tmp/rpm_pid_test
// /tmp/rpm_pid_test
#include "rpm_setpoint_pid.h"
#include <cassert>
#include <cmath>
#include <limits>
#include <initializer_list>

using namespace DriveConstants;

static RPMSetpointPID::Tuning tuning() {
  return {RPM_PID_LOW_SPEED_TAU_S, RPM_PID_HIGH_SPEED_TAU_S,
          RPM_PID_KI, RPM_PID_KD, RPM_PID_DERIVATIVE_FILTER_S,
          RPM_PID_LOW_SPEED_ACCEL_MS2, RPM_PID_HIGH_SPEED_ACCEL_MS2,
          RPM_PID_MAX_JERK_RPM_PER_S2};
}

int main() {
  constexpr float dt = .005f;
  const float designRPM = MAX_SPEED * LINEAR_TO_RPM;
  // Above-design selections behave exactly like selections at design limits.
  RPMSetpointPID hard(tuning()), excessive(tuning());
  for (int k = 0; k < 4000; ++k) {
    const float target = k < 2000 ? 1000.0f : -1000.0f;
    const float a = hard.smoothRPM(target, dt, MAX_SPEED, MAX_ACCEL);
    const float b = excessive.smoothRPM(target, dt, 100.0f, 100.0f);
    assert(a == b);
    assert(std::fabs(a) <= designRPM);
  }

  // Low selections constrain both signed output RPM and acceleration.
  RPMSetpointPID low(tuning());
  float previous = 0.0f;
  for (int k = 0; k < 2000; ++k) {
    const float rpm = low.smoothRPM(k < 1000 ? 500.0f : -500.0f, dt, 1.0f, .3f);
    assert(std::fabs(rpm) <= 1.0f * LINEAR_TO_RPM);
    // Skip a hard output-boundary crossing: that clamp has higher priority.
    if (std::fabs(rpm) < LINEAR_TO_RPM && std::fabs(previous) < LINEAR_TO_RPM) {
      assert(std::fabs(rpm - previous) / dt / LINEAR_TO_RPM <= .3001f);
    }
    previous = rpm;
  }

  // An acceleration knob reduction acts immediately, despite stored acceleration.
  RPMSetpointPID changing(tuning());
  previous = 0.0f;
  for (int k = 0; k < 160; ++k)
    previous = changing.smoothRPM(1000.0f, dt, MAX_SPEED, MAX_ACCEL);
  const float afterAccelReduction = changing.smoothRPM(1000.0f, dt, MAX_SPEED, .1f);
  assert(std::fabs(afterAccelReduction - previous) / dt / LINEAR_TO_RPM <= .1001f);
  // Negative acceleration is subject to the same immediate reduction.
  RPMSetpointPID reverse(tuning());
  float previousReverse = 0.0f;
  for (int k = 0; k < 160; ++k)
    previousReverse = reverse.smoothRPM(-1000.0f, dt, MAX_SPEED, MAX_ACCEL);
  const float reducedReverse = reverse.smoothRPM(-1000.0f, dt, MAX_SPEED, .1f);
  assert(std::fabs(reducedReverse - previousReverse) / dt / LINEAR_TO_RPM <= .1001f);
  // Raising the cap still respects jerk rather than jumping to the new cap.
  const float oldAccelRPM = (afterAccelReduction - previous) / dt;
  const float raisedRPM = changing.smoothRPM(1000.0f, dt, MAX_SPEED, MAX_ACCEL);
  const float newAccelRPM = (raisedRPM - afterAccelReduction) / dt;
  assert(std::fabs(newAccelRPM - oldAccelRPM) <= RPM_PID_MAX_JERK_RPM_PER_S2 * dt + .001f);
  // Lower speed bounds apply to the returned value immediately in either direction.
  const float afterSpeedReduction = changing.smoothRPM(1000.0f, dt, .2f, .1f);
  assert(std::fabs(afterSpeedReduction) <= .2f * LINEAR_TO_RPM);

  // Raising a selection changes acceleration, not current RPM discontinuously.
  const float afterIncrease = changing.smoothRPM(1000.0f, dt, MAX_SPEED, MAX_ACCEL);
  assert(std::fabs(afterIncrease - afterSpeedReduction) / dt / LINEAR_TO_RPM <= MAX_ACCEL + .0001f);

  // Zero, negative and nonfinite limits clear state, as do invalid dt/request.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  for (float bad : {0.0f, -1.0f, nan, inf}) {
    RPMSetpointPID pid(tuning());
    pid.smoothRPM(100.0f, dt, MAX_SPEED, MAX_ACCEL);
    assert(pid.smoothRPM(100.0f, dt, bad, MAX_ACCEL) == 0.0f);
    assert(pid.smoothRPM(100.0f, dt, MAX_SPEED, bad) == 0.0f);
    assert(pid.smoothRPM(0.0f, dt, MAX_SPEED, MAX_ACCEL) == 0.0f);
  }
  assert(changing.smoothRPM(nan, dt, MAX_SPEED, MAX_ACCEL) == 0.0f);
  assert(changing.smoothRPM(100.0f, 0.0f, MAX_SPEED, MAX_ACCEL) == 0.0f);

  // Targets remain unclipped; output alone is speed-limited. A small speed
  // selection must not replace the requested RPM in the proportional error.
  auto fast = tuning();
  fast.lowSpeedTauS = fast.highSpeedTauS = 1.0f;
  fast.ki = fast.kd = 0.0f;
  fast.maxJerkRPMPerS2 = 1.0e6f;
  RPMSetpointPID unclipped(fast);
  const float first = unclipped.smoothRPM(2.0f, .001f, .01f, MAX_ACCEL);
  assert(std::fabs(first - .002f) < 1.0e-6f);

  // Independent wheels and explicit reset.
  RPMSetpointPID left(tuning()), right(tuning());
  assert(left.smoothRPM(100.0f, dt, MAX_SPEED, MAX_ACCEL) > 0.0f);
  assert(right.smoothRPM(0.0f, dt, MAX_SPEED, MAX_ACCEL) == 0.0f);
  left.reset();
  assert(left.smoothRPM(0.0f, dt, MAX_SPEED, MAX_ACCEL) == 0.0f);
}
