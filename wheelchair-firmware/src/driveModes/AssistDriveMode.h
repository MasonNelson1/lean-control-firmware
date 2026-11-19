#pragma once

#include "DriveMode.h"
#include <cstdint>
#include "PushAssist.h"

class AssistDriveMode : public DriveMode
{
private:
    float leftRPM, rightRPM;
    uint32_t now;
    float maxRPM;
    PushAssist leftAssist, rightAssist;

public:
    MotorTargets getTargets() override;
    void loop(float leftRPM, float rightRPM, float maxRPM)
    {
        AssistDriveMode::leftRPM = leftRPM;
        AssistDriveMode::rightRPM = rightRPM;
        AssistDriveMode::maxRPM = maxRPM;
    }
};
