
#pragma once

#include "DriveMode.h"
#include "PushAssist.h"

struct LeanTelemetry
{
    int front;
    int back;
    int left;
    int right;
    float speed;
    float turn;
    bool stopped = true;
};

class LeanDriveMode : public DriveMode
{
public:
    struct WheelCommand
    {
        float value; // Amps in current control, normalized input in velocity control.
        bool currentControl = false;
    };

    struct WheelCommands
    {
        WheelCommand left;
        WheelCommand right;
    };

private:
    struct WheelAssist
    {
        PushAssist boost;
        bool active = false;

        WheelCommand command(float input, bool useAssist, float measuredRPM,
                             float maxRPM, uint32_t now);
    };

    WheelAssist leftAssist, rightAssist;
    bool initiallyCalibrated = false;
    LeanTelemetry telemetry{};

    int floorF = 0;
    int floorB = 0;
    int floorL = 0;
    int floorR = 0;

    int userWeightEstimate = 370; // Unloaded sensor estimate until calibration.

    // Cached from the weight estimate and configured factors during calibration.
    int frontSaturation = 0;
    int backSaturation = 0;
    int leftSaturation = 0;
    int rightSaturation = 0;

    float speedResponseExponent = 2.0f; // default initializer for response curve, should be overwritten before its ever used.
    float turnResponseExponent = 2.0f;

public:
    void reset()
    {
        initiallyCalibrated = false;
        leftAssist.active = false;
        rightAssist.active = false;
    }
    MotorTargets getTargets() override;
    WheelCommands getWheelCommands(float leftRPM, float rightRPM, float maxRPM, uint32_t now);
    // Refresh sensor telemetry without generating motor commands or changing LEDs.
    void sampleSensors();
    void calibrate();
    void updateResponseCurve();
    const LeanTelemetry &getTelemetry() const { return telemetry; }
    int getUserWeightEstimate() const { return userWeightEstimate; }
};
