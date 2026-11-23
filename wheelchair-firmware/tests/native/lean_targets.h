// council-corner: LeanDriveMode no longer mixes speed and turn into wheel
// shares (that was getTargets()/getTargetsFromSample() with the "Lean turn
// mixing" curve; DriveController now takes speed and turn separately, turn as
// a rate). These helpers keep dev-setup's lean tests reading the same way:
// left = speed + turn, right = speed - turn, each clamped to ±1, with no turn
// mix. A stopped command is {0, 0}.
#pragma once
#include "driveModes/LeanDriveMode.h"
#include <algorithm>

inline MotorTargets wheelShares(const LeanDriveMode::Command &c)
{
    if (c.stopped)
        return {0.0f, 0.0f};
    return {std::clamp(c.speed + c.turn, -1.0f, 1.0f), std::clamp(c.speed - c.turn, -1.0f, 1.0f)};
}
// Sample the sensors, then the command (what getTargets() did).
inline MotorTargets getTargets(LeanDriveMode &mode)
{
    mode.sampleSensors();
    return wheelShares(mode.command());
}
// The command from the last sample (what getTargetsFromSample() did).
inline MotorTargets getTargetsFromSample(LeanDriveMode &mode) { return wheelShares(mode.command()); }
