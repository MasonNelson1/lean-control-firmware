#pragma once

#include "DriveMode.h"
#include <cstdint>
#include "PushAssist.h"

class AssistDriveMode : public DriveMode
{
private:
    float leftRPM, rightRPM;
    float maxRPM;
    PushAssist leftAssist, rightAssist;

public:
    MotorTargets getTargets() override;
    void reset()
    {
        leftAssist.reset();
        rightAssist.reset();
    }

    void loop(float leftRPM, float rightRPM, float maxRPM)
    {
        AssistDriveMode::leftRPM = leftRPM;
        AssistDriveMode::rightRPM = rightRPM;
        AssistDriveMode::maxRPM = maxRPM;
    }
};
