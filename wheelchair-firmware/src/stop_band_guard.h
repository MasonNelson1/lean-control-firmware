#pragma once

// Stop-band guard: never send a near-stop RPM setpoint to a wheel that is still
// rolling (court 2026-11-22, ABS_OVER_CURRENT at 17:08 right and 18:03 left).
//
// The VESC forces duty to 0 (shorts the motor) for any speed setpoint under
// s_pid_min_erpm; at speed that short overshoots the absolute-current trip and the
// VESC lets the wheel go for ~0.6 s. So while a wheel rolls faster than
// STOP_GUARD_RELEASE_RPM, a command inside the stop band (|cmd| < FLOOR) or of the
// opposite sign becomes +-STOP_GUARD_FLOOR_RPM in the wheel's own direction, and
// the VESC's speed loop brakes the wheel (current- and battery-limited). Once the
// wheel is slow the command passes through unchanged, so 0 (a short at ~14 A or
// less) and reversing are allowed again. Constants and their reasons:
// drive_constants.h.
//
// Applied per wheel to the final RPM command in setMotorPower (LEAN and REMOTE),
// after the turning logic. Brake-current paths (safety brake, bench, calibration,
// disarm) don't go through it.
//
// Signs: wheel RPM is forward-positive on BOTH sides. The right VESC is mirrored
// in hardware but has m_invert_direction = 1, so its CAN ERPM and setpoints are
// already forward-positive (11/22 logs: both wheels read ~+63 RPM driving forward).
//
// Pure logic, no Arduino. Tests: tests/native/stop_band_guard_test.cpp.

#include <cmath>
#include <cstdint>

#include "drive_constants.h"

namespace stopguard
{
class Wheel
{
public:
    // command, measured: wheel RPM (forward positive). fresh: the measured RPM is
    // recent; a stale reading never arms the guard (a VESC that stopped reporting
    // mustn't be held at the floor speed forever).
    float apply(float command, float measured, bool fresh)
    {
        const float speed = std::fabs(measured);
        if (!fresh || speed < common::STOP_GUARD_RELEASE_RPM)
            rolling_ = false;
        else if (speed > common::STOP_GUARD_ENGAGE_RPM)
            rolling_ = true;

        const bool nearStop = std::fabs(command) < common::STOP_GUARD_FLOOR_RPM;
        const bool reversed = command * measured < 0.0f;
        const bool engage = rolling_ && (nearStop || reversed);
        if (engage && !engaged_)
            ++engagements_;
        engaged_ = engage;
        if (!engage)
            return command;
        return measured > 0.0f ? common::STOP_GUARD_FLOOR_RPM : -common::STOP_GUARD_FLOOR_RPM;
    }

    // Forget the rolling state (the mode no longer sends RPM commands).
    void reset()
    {
        rolling_ = false;
        engaged_ = false;
    }

    bool rolling() const { return rolling_; }         // armed (hysteresis on |measured|)
    bool engaged() const { return engaged_; }         // the last apply() replaced the command
    uint32_t engagements() const { return engagements_; } // off->on transitions since boot

private:
    bool rolling_ = false;
    bool engaged_ = false;
    uint32_t engagements_ = 0;
};

// Rate limit for the DBG EVT stop-guard lines: one line per change of the engaged
// state, at most one per MinGapMs. A change inside the gap is reported once the gap
// has passed if the state still differs from the last line, so the log always ends
// on the true state; the line's n= (engagements since boot) shows any skipped flips.
class LogGate
{
public:
    static constexpr uint32_t MinGapMs = 100;

    // True when a line should go out now for this state; it is then the logged state.
    bool due(bool engaged, uint32_t nowMs)
    {
        if (engaged == logged_ || (haveLine_ && nowMs - lastMs_ < MinGapMs))
            return false;
        logged_ = engaged;
        haveLine_ = true;
        lastMs_ = nowMs;
        return true;
    }

private:
    bool logged_ = false;
    bool haveLine_ = false;
    uint32_t lastMs_ = 0;
};
}
