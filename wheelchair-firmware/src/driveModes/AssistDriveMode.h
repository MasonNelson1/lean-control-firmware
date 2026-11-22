#pragma once

#include "DriveMode.h"
#include <cstdint>
#include "PushAssist.h"

// Push assist: each motor adds current in the direction its wheel is already
// being pushed. Needs live wheel speed from the VESCs.
class AssistDriveMode
{
private:
    PushAssist leftAssist, rightAssist;

public:
    // Boost current (A) per motor for these measured wheel speeds. maxRPM is
    // the speed-knob limit: the boost fades out as a wheel approaches it.
    MotorTargets getTargets(float leftRPM, float rightRPM, float maxRPM, uint32_t now)
    {
        return {leftAssist.update(leftRPM, now, maxRPM), rightAssist.update(rightRPM, now, maxRPM)};
    }

    void reset()
    {
        leftAssist.reset();
        rightAssist.reset();
    }
};
