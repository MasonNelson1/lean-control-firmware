
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

    float frontSaturation = 0;
    float backSaturation = 0;
    float leftSaturation = 0;
    float rightSaturation = 0;

    // these constants decide how much a sensor needs to be triggered in relation to
    // the weight estimate to be considered full input.
    // so a value of one means if the sensor is reading a value equal to the user's weight, the the max value is reached.
    // the smaller the value, the more responsive the chair is.
    int frontWeightFactor = 1;
    int backWeightFactor = 1;
    int leftWeightFactor = 1;
    int rightWeightFactor = 1;

public:
    MotorTargets getTargets() override;
    void calibrate();
    const LeanTelemetry &getTelemetry() const { return telemetry; }
    int getUserWeightEstimate() const { return userWeightEstimate; }
};
