
#pragma once

#include "DriveMode.h"
#include "../telemetry/TelemetryTypes.h"
#include <array>

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
    bool railed = false;
    std::array<int, 4> rawReadings{}; // last raw ADC counts, front/back/left/right
    int userWeightEstimate = 370; // Unloaded sensor estimate until calibration.

    float speedResponseExponent = 2.0f; // default initializer for response curve, should be overwritten before its ever used.
    float turnResponseExponent = 2.0f;

    enum class CalibrationState { Idle, Waiting, Collecting };
    CalibrationState calibrationState = CalibrationState::Idle;
    uint32_t calibrationPressedAt = 0;
    uint32_t collectionStartedAt = 0;
    // Sensor order is front, back, left, right throughout calibration.
    std::array<float, 4> dynamicGains{{1, 1, 1, 1}};
    std::array<int, 4> flooredReadings{};
    std::array<int, 4> calibrationPeaks{};
    std::array<int, 4> oppositeAtPeak{};
    void finishDynamicCalibration();

public:
    void reset() { initiallyCalibrated = false; cancelCalibration(); }
    MotorTargets getTargets() override;
    MotorTargets getTargetsFromSample();
    bool isSeated() const { return 2 * seatLoad > userWeightEstimate; }
    // True when the last sample had any raw reading at an ADC rail (shorted or open sensor).
    bool sensorAtRail() const { return railed; }
    // Refresh the seat reading and optionally refresh lean telemetry.
    void sampleSensors(bool seatOnly = false);
    void calibrate();
    void beginCalibration(uint32_t pressedAt);
    // Returns true when releasing the button finishes a dynamic calibration attempt.
    bool updateCalibration(uint32_t now, bool buttonHeld);
    void cancelCalibration();
    bool calibrationActive() const { return calibrationState != CalibrationState::Idle; }
    bool calibrationLEDOn(uint32_t now) const;
    void resetDynamicGains();
    void publishDynamicGains() const;
    // Once-a-second debug lines: raw ADC counts and calibration state.
    void publishSensorStatus() const;
    const std::array<float, 4> &getDynamicGains() const { return dynamicGains; }
    void updateResponseCurve();
    const telemetry::SensorTelemetry &getTelemetry() const { return telemetry; }
    int getUserWeightEstimate() const { return userWeightEstimate; }
};
