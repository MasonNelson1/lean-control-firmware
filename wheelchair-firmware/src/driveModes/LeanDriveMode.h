
#pragma once

#include "DriveMode.h"

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
private:
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

    float responseExponent = 2.0f; // default initializer for response curve, should be overwritten before its ever used.

public:
    MotorTargets getTargets() override;
    void calibrate();
    void updateResponseCurve();
    const LeanTelemetry &getTelemetry() const { return telemetry; }
    int getUserWeightEstimate() const { return userWeightEstimate; }
};
