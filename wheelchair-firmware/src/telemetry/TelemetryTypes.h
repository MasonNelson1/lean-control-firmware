#pragma once

#include "../Mode.h"
#include "../driveModes/DriveMode.h"
#include <cstdint>

namespace telemetry
{
    struct SensorTelemetry
    {
        int front;
        int back;
        int left;
        int right;
        float speed;
        float turn;
        bool stopped = true;
    };

    struct RemoteTelemetry
    {
        bool stopped = true;
    };

    struct MotorCommands
    {
        float leftTargetRPM, rightTargetRPM, leftSmoothedRPM, rightSmoothedRPM;
    };

    struct MotorFeedback
    {
        float rpm = 0;
        uint32_t lastRpmMs = 0;
        bool received = false;
        float actualRPM(uint32_t now) const;
    };

    // A complete sample, independent of serial ports and drive-mode lifetimes.
    struct TelemetryFrame
    {
        uint32_t timeMs = 0;
        common::mode mode = common::DISARM;
        SensorTelemetry lean{};
        int userWeight = 0;
        bool remoteStopped = true;
        MotorCommands commands{};
        MotorFeedback left, right;
        MotorTargets boostCurrent{};
    };

}
