#pragma once

#include <stdint.h>

namespace common {
constexpr float WHEEL_CIRCUMFERENCE_M = 74.75f * 0.0254f;
// Shared derived value, refreshed when the dashboard changes wheel circumference.
inline float LINEAR_TO_RPM = 60.0f / WHEEL_CIRCUMFERENCE_M;
constexpr float MAX_ACCEL = 2.85f * 1.3f;      // m/s^2
constexpr float MAX_SPEED = 4.91744f;   // m/s (x% of 11 mph for testing).
constexpr float MAX_RPM_COMMAND_DIFF = 50;//the furthest that the rpm setpoint can be from the actual motor setpoint, helps to prevent overcurrent faults.

// ---- Stopping (StopController.h) -------------------------------------------
// Every stop (DISARM button, empty seat, sensor fault, silent VESC, lost
// remote) ramps the wheel-speed command straight to zero at this rate, with no
// filter lag or jerk limit, then finishes on brake current.
//
// It equals MAX_ACCEL on purpose: that is the hardest braking the chair already
// does in normal driving, so it is known not to trip VESC over-current faults
// or pitch the rider. Stop times from each speed-knob position: 0.33 s, 0.66 s,
// 1.0 s, 1.33 s (CDR Spec P allows 3.0 s).
// CDR Spec B (command below 5% of max speed within 0.5 s) would need
// 0.95 * MAX_SPEED / 0.5 = 9.3 m/s^2 from top speed, more than the motors can
// deliver (CDR estimate ~4 m/s^2). Raising this is a team decision.
constexpr float STOP_DECEL = MAX_ACCEL; // m/s^2

// A VESC in speed mode lets go of the motor below s_pid_min_erpm (VESC Tool
// default 900 ERPM = 15.5 wheel RPM), so the last part of a stop would coast.
// Below this wheel speed the stop switches from an RPM command to brake
// current. 18 RPM is 1047 ERPM, a margin above the VESC default of 900.
constexpr float STOP_HANDOFF_RPM = 18.0f;
// Brake current per motor for that last part. The assist boost already runs up
// to 30 A per motor, so 15 A is well inside what the VESCs are set up for.
constexpr float STOP_BRAKE_AMPS = 15.0f;
// The wheels count as stopped below this speed, once held for STOP_SETTLE_MS.
constexpr float STOP_DONE_RPM = 3.0f;
constexpr uint32_t STOP_SETTLE_MS = 150;
// Longest the brake phase lasts. With no VESC feedback it always runs this long.
constexpr uint32_t STOP_BRAKE_MAX_MS = 1500;
// If the wheels are not following the ramp, keep commanding the handoff speed
// for at most this long after the ramp ends before going to brake current.
constexpr uint32_t STOP_RAMP_OVERRUN_MS = 1000;

// ---- Motor controller health -----------------------------------------------
// A VESC whose speed frames stop for this long is treated as silent. VESC Tool
// sends status at 50 Hz by default (20 ms), so this is ten missed frames. The
// HEALTH line reports the measured rate; if it shows 50 Hz this can come down
// toward the 100 ms of CDR Spec B.
constexpr uint32_t VESC_STALE_MS = 200;

// ---- Turning (MotionSmoother.h) --------------------------------------------
// How fast the chair's turn rate may build up and die away, in deg/s per
// second. Turning is smoothed on its own axis so it no longer waits behind the
// forward acceleration limit. CDR Spec H asks for 200 deg/s within 0.5 s, i.e.
// at least 400. Releasing a lean stops the rotation faster than it built up,
// which is what lets the rider stop on an angle: from 100 deg/s the chair
// turns a further 100^2 / (2 * 900) = 5.6 degrees.
constexpr float TURN_ACCEL_DEG_S2 = 600.0f;
constexpr float TURN_DECEL_DEG_S2 = 900.0f;

}

namespace DriveConstants = common;
