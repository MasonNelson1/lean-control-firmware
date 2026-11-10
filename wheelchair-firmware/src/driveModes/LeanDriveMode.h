
#pragma once

#include "DriveMode.h"

class LeanDriveMode : public DriveMode
{
private:
    bool initiallyCalibrated = false;

    int floorF = 0;
    int floorB = 0;
    int floorL = 0;
    int floorR = 0;

    int userWeightEstimate = 0;

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

    bool stopped = true;

public:
    MotorTargets getTargets() override;
    void calibrate();
};
