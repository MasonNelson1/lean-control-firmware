#pragma once

namespace common {
constexpr float WHEEL_CIRCUMFERENCE_M = 74.75f * 0.0254f;
// Shared derived value, refreshed when the dashboard changes wheel circumference.
inline float LINEAR_TO_RPM = 60.0f / WHEEL_CIRCUMFERENCE_M;
constexpr float MAX_ACCEL = 2.85f * 1.3f;      // m/s^2
constexpr float MAX_SPEED = 4.91744f;   // m/s (x% of 11 mph for testing).
constexpr float MAX_RPM_COMMAND_DIFF = 50;//the furthest that the rpm setpoint can be from the actual motor setpoint, helps to prevent overcurrent faults.

// Stop-band guard (stop_band_guard.h), court 2026-11-22 ABS_OVER_CURRENT at 17:08 (R)
// and 18:03 (L). In speed mode the VESC forces duty to 0 for any setpoint under
// s_pid_min_erpm (mcpwm_foc.c "Brake when set ERPM is below min ERPM"): it shorts
// the motor, and the current scales with wheel speed (~40 A per m/s here: ~58 A
// at 1.5 m/s, past the 49/52 A trip; ~14 A at 0.35 m/s). So a wheel still rolling
// faster than the release speed is commanded +-FLOOR (in its direction) instead
// of a near-zero or reversed setpoint, and the VESC's speed loop (current- and
// battery-limited) brakes it. 1 wheel RPM = 0.0319 m/s = 58.2 ERPM.
//  - FLOOR 8 RPM = 465 ERPM, 1.55x s_pid_min_erpm = 300 (5.2 RPM). Both VESCs read
//    back 300 from flash in the 11/22 logs (left 18:06, right 21:48). If a VESC were
//    ever reset to the stock 900 (15.5 RPM), 8 would sit inside its stop band again:
//    no worse than before this guard, but re-check after any VESC reconfiguration.
//  - FLOOR must stay well below RELEASE: the speed loop pulls the wheel down to
//    FLOOR, and the guard lets go only once the wheel is under RELEASE. A floor at or
//    above RELEASE would hold the chair rolling at the floor speed and never stop it.
//  - RELEASE 11 RPM (0.35 m/s): below it a 0 setpoint is safe (~14 A short).
//  - ENGAGE 14 RPM (0.45 m/s, ~17 A short): the guard arms above this and disarms
//    below RELEASE, so it doesn't chatter at the threshold.
constexpr float STOP_GUARD_FLOOR_RPM = 8.0f;
constexpr float STOP_GUARD_RELEASE_RPM = 11.0f;
constexpr float STOP_GUARD_ENGAGE_RPM = 14.0f;
static_assert(STOP_GUARD_FLOOR_RPM + 2.0f <= STOP_GUARD_RELEASE_RPM, "the speed loop must be able to pull the wheel under RELEASE");
static_assert(STOP_GUARD_RELEASE_RPM < STOP_GUARD_ENGAGE_RPM, "hysteresis");

}

namespace DriveConstants = common;
