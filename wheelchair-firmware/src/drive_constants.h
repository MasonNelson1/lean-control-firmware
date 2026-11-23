#pragma once

#include <stdint.h>

// ---- VESC Tool settings this firmware relies on -----------------------------
// They live in the VESCs and cannot be read or set from here. Check them on
// both VESCs on the bench, wheels off the ground:
//
//   Speed PID, "Allow Braking": ON. All normal slowing down and the first part
//     of every stop are speed commands. With it off the VESC only coasts.
//     (A stop notices wheels that do not slow and switches to brake current,
//     and logs it; normal driving cannot.)
//   Speed PID, "Minimum ERPM" (s_pid_min_erpm): default 900 = 15.5 wheel RPM.
//     Below it the VESC does not drive, so a small lean does nothing and then
//     the chair starts at 0.5 m/s. Lower it as far as the motor's sensors
//     allow, check that a 5 RPM command is followed, then set "VESC minimum
//     speed" on the dashboard to match.
//   App, "Timeout": 200 ms, and "Timeout Brake Current": 15 A. These decide
//     what the chair does if this firmware hangs (see watchdog.h).
//   App, CAN status rate: 50 Hz. Under 25 Hz the chair faults as "VESC silent".
//   Speed PID, "Ramp ERPMs per second" (s_pid_ramp_erpms_s): the VESC's own
//     limit on how fast a speed command may change. Keep it at its default
//     (25000) or higher, or it slows every stop.
//   Bring-up check, wheels off: lean right, and the LEFT wheel must run faster.
//     If it is the other way round the two VESC IDs are swapped.
//   VESC firmware version: note it. Below the minimum ERPM, current versions
//     brake with shorted phases and older ones release the motor.

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
// In normal driving, a wheel that turns faster than a command inside the
// VESC's minimum-speed zone is slowed with brake current in proportion to
// the excess: this many amps per wheel RPM, between the two limits. 15 RPM too
// fast is the full 15 A; close to rest it eases off to 3 A instead of
// grabbing, and it lets go below DEADZONE_REST_RPM (0.03 m/s).
constexpr float DEADZONE_BRAKE_AMPS_PER_RPM = 1.0f;
constexpr float DEADZONE_BRAKE_MIN_AMPS = 3.0f;
constexpr float DEADZONE_REST_RPM = 1.0f;
// The wheels count as stopped below this speed, once held for STOP_SETTLE_MS.
constexpr float STOP_DONE_RPM = 3.0f;
constexpr uint32_t STOP_SETTLE_MS = 150;
// How long the brake phase lasts with no VESC feedback to say the wheels have
// stopped.
constexpr uint32_t STOP_BRAKE_MAX_MS = 1500;
// With feedback it lasts until the wheels have stopped, but no longer than this.
constexpr uint32_t STOP_BRAKE_LIMIT_MS = 5000;
// During the ramp the wheels are checked this often. If they are well above
// the ramp and have slowed by less than a fifth of what STOP_DECEL asks, the
// VESC's speed loop is not braking (its "Allow Braking" is off, or it is not
// acting on the commands) and the stop goes to brake current at once.
constexpr uint32_t STOP_RAMP_CHECK_MS = 200;
constexpr float STOP_RAMP_LAG_RPM = 10.0f;
// ...and only if the motors report less current than this, i.e. are not braking.
constexpr float STOP_RAMP_IDLE_AMPS = 3.0f;
// If the wheels are not following the ramp, keep commanding the handoff speed
// for at most this long after the ramp ends before going to brake current.
constexpr uint32_t STOP_RAMP_OVERRUN_MS = 1000;

// ---- Calibration -----------------------------------------------------------
// The calibration button is ignored while a wheel turns faster than this
// (0.63 m/s). Below it the chair is stopped first, then calibrated.
constexpr float CALIBRATE_MAX_RPM = 20.0f;

// ---- Reverse ---------------------------------------------------------------
// Full back lean (or full back stick) drives at this share of the speed-knob
// limit. The rider cannot see behind, and leaning back is also how they brake.
// Half keeps reverse usable on the slowest knob position, where it is 19 wheel
// RPM: just above the 15.5 RPM under which a VESC at its default minimum-ERPM
// setting will not drive.
constexpr float REVERSE_SPEED_FRACTION = 0.5f;

// ---- Neutral trim (LeanDriveMode::trimNeutral) ------------------------------
// The chair must have stood still at neutral lean for this long before neutral
// starts to follow the sensors.
constexpr float NEUTRAL_TRIM_AFTER_S = 2.0f;

// ---- Motor controller health -----------------------------------------------
// A VESC whose speed frames stop for this long is treated as silent. With one
// control tick on top that is inside the 0.1 s of CDR Spec B. VESC Tool sends
// status at 50 Hz by default (20 ms), so this is four missed frames. It needs
// the status rate to stay at 25 Hz or more; the HEALTH line reports the
// measured rate (vescL / vescR).
constexpr uint32_t VESC_STALE_MS = 80;

// ---- Turning (MotionSmoother.h) --------------------------------------------
// Default for the "Track width" setting. NOT MEASURED: the council's guess.
// Too small makes the chair turn slower than the turn-rate setting asks.
// Measure contact patch to contact patch and set it on the dashboard.
constexpr float TRACK_WIDTH_ESTIMATE_M = 0.60f;
// How fast the chair's turn rate may build up and die away, in deg/s per
// second. Turning is smoothed on its own axis so it no longer waits behind the
// forward acceleration limit. CDR Spec H asks for 200 deg/s within 0.5 s, i.e.
// at least 400. Releasing a lean stops the rotation faster than it built up,
// which is what lets the rider stop on an angle: from the full 160 deg/s the
// command turns a further 160^2 / (2 * 1800) = 7 degrees. This is the constant
// to tune wheels-off if stopping a turn feels too sharp or too loose. The chair itself takes a
// little longer over the last part, where the wheels are slowed on brake
// current (DEADZONE_BRAKE_* below).
constexpr float TURN_ACCEL_DEG_S2 = 600.0f;
constexpr float TURN_DECEL_DEG_S2 = 1800.0f;

}

namespace DriveConstants = common;
