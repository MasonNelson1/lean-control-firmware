
#pragma once

#include "DriveMode.h"
#include "lean_layout.h"
#include "../telemetry/TelemetryTypes.h"
#include <array>

// Lean steering. The sensor layout setting (settings::sensorLayout) picks how a
// sample becomes speed and turn; see lean_layout.h for the math.
//
// Plus layout: floors are the rider's neutral readings, speed is (F−B)/W and
//   turn is (R−L)/W after per-sensor gains. Dynamic calibration sets the gains.
// Corner layout: speed and turn come from the rider's centre of pressure (CoP),
//   measured from where it sat at calibration. Dynamic calibration sets how far
//   the rider can reach in each direction.
//
// Both layouts share everything after that: the response curves, reverse-turn
// flip and turn mixing here, then the mode state machine, RPM PID and clamps
// in main.cpp. Seat detection (Σ raw − default floor vs W) is layout-independent.
class LeanDriveMode : public DriveMode
{
private:
    bool initiallyCalibrated = false;
    // Layout the current calibration was taken with. Floors and neutral only mean
    // something for that layout, so a different live layout keeps LEAN stopped.
    lean::SensorLayout calibratedLayout = lean::SensorLayout::Plus;
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

    // Corner layout state.
    lean::CentreOfPressure cop{};     // last sample; invalid on the plus layout
    lean::CentreOfPressure neutral{}; // the rider's active neutral, from calibrate()
    lean::Reach calibratedReach{};    // from a dynamic calibration, when reachCalibrated
    bool reachCalibrated = false;
    lean::Reach reachPeaks{};         // furthest travel seen during dynamic calibration

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
    void finishReachCalibration();
    void sampleCorner();
    bool readyToDrive() const;

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
    // Drops the dynamic calibration: plus gains back to the configured gains,
    // corner reach back to the configured reach. Called at startup and on a
    // profile change.
    void resetDynamicGains();
    // DYNAMIC_GAINS line: the gains the active layout applies per channel.
    void publishDynamicGains() const;
    // Once-a-second debug lines: raw ADC counts and calibration state.
    void publishSensorStatus() const;
    // 20 Hz DBG COP line on the corner layout; nothing on the plus layout.
    void publishCentreOfPressure(uint32_t now) const;
    const std::array<int, 4> &getRawReadings() const { return rawReadings; }
    const std::array<float, 4> &getDynamicGains() const { return dynamicGains; }
    void updateResponseCurve();
    const telemetry::SensorTelemetry &getTelemetry() const { return telemetry; }
    int getUserWeightEstimate() const { return userWeightEstimate; }

    static lean::SensorLayout layout();
    const lean::CentreOfPressure &getCentreOfPressure() const { return cop; }
    const lean::CentreOfPressure &getNeutral() const { return neutral; }
    // Dynamic-calibration reach if one was taken, otherwise the configured reach.
    lean::Reach activeReach() const;
};
