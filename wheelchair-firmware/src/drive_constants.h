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
// or pitch the rider. The ramp ends at STOP_HANDOFF_RPM, where brake current
// takes over and no speed is commanded any more.
//
// CDR Spec B asks for the speed command to be under 5% of maximum within 0.5 s
// of a fault. Time from the start of a stop until the speed command ends, by
// speed-knob position: 0.23 s, 0.56 s, 0.89 s, 1.22 s. So Spec B is met on
// position 1, just missed on position 2 and missed on 3 and 4. Meeting it from
// top speed needs about 2.3 times this rate (8.7 m/s^2), more than the motors
// can deliver (CDR estimate ~4 m/s^2). Raising this is a team decision.
// CDR Spec P (stopped within 3.0 s) is met with margin on every position.
constexpr float STOP_DECEL = MAX_ACCEL; // m/s^2
// The deceleration builds up over this long instead of arriving in one step
// (0.38 g at once pitches the rider forward). It adds half this to a stop.
constexpr float STOP_ONSET_S = 0.1f;

// A VESC in speed mode stops running its speed loop when the command is under
// s_pid_min_erpm (VESC Tool default 900 ERPM = 15.5 wheel RPM). What it does
// instead depends on its firmware: current versions short the motor phases,
// which brakes ("Brake when set ERPM is below min ERPM" in bldc's
// mcpwm_foc.c); older ones release the motor, which coasts. A stop must not
// depend on which, so below this wheel speed it switches from an RPM command
// to brake current. Brake-current mode never drives in reverse and, near a
// standstill, shorts the phases (same source file). It is the "VESC minimum speed" setting plus this margin, refreshed
// when settings change: 18 RPM (1047 ERPM) with the VESC default.
constexpr float STOP_HANDOFF_MARGIN_RPM = 2.5f;
inline float STOP_HANDOFF_RPM = 15.5f + STOP_HANDOFF_MARGIN_RPM;
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
