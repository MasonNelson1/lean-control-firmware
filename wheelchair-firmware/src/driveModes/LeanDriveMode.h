
#pragma once

#include "DriveMode.h"
#include "../telemetry/TelemetryTypes.h"

class LeanDriveMode : public DriveMode
{
private:
    bool initiallyCalibrated = false;
    telemetry::SensorTelemetry telemetry{};

    int floorF = 0;
    int floorB = 0;
    int floorL = 0;
    int floorR = 0;

    int seatLoad = 0;
    int userWeightEstimate = 370; // Unloaded sensor estimate until calibration.

    // Cached from the weight estimate and configured factors during calibration.
    int frontSaturation = 0;
    int backSaturation = 0;
    int leftSaturation = 0;
    int rightSaturation = 0;

    float speedResponseExponent = 2.0f; // default initializer for response curve, should be overwritten before its ever used.
    float turnResponseExponent = 2.0f;

public:
    void reset() { initiallyCalibrated = false; }
    MotorTargets getTargets() override;
    MotorTargets getTargetsFromSample();
    bool isSeated() const { return 2 * seatLoad > userWeightEstimate; }
    // Refresh the seat reading and optionally refresh lean telemetry.
    void sampleSensors(bool seatOnly = false);
    void calibrate();
    void updateResponseCurve();
    const telemetry::SensorTelemetry &getTelemetry() const { return telemetry; }
    int getUserWeightEstimate() const { return userWeightEstimate; }
};
