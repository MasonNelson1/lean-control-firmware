
#include "AssistDriveMode.h"

MotorTargets AssistDriveMode::getTargets()
{
    const uint32_t now = millis();
    return {leftAssist.update(leftRPM, now, maxRPM), rightAssist.update(rightRPM, now, maxRPM)};
}
