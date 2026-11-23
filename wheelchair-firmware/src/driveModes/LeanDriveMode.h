
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
//   measured from where it sat at calibration (and, after a second press, from
//   the relaxed posture too). Dynamic calibration sets how far the rider can
//   reach in each direction. The empty-plate baseline re-zeroes itself while
//   the chair is disarmed and the seat is empty.
//
// Both layouts share everything after that: the response curves, reverse-turn
// flip and turn mixing here, then the mode state machine, RPM PID and clamps
// in main.cpp. Seat detection (Σ raw − default floor vs W) is layout-independent.
class LeanDriveMode : public DriveMode
{
public:
    // main.cpp sets this each tick: reference speed / speed-knob cap (corner layout turn).
    void setTurnKnobScale(float scale) { turnKnobScale = scale; }
    // True once after a hold calibration wrote new reach settings (main.cpp saves them to the active profile).
    bool takeReachSave() { const bool p = reachSavePending; reachSavePending = false; return p; }
    // Raw 12-bit counts outside [SensorRailLow, SensorRailHigh] mean a shorted,
    // open or unplugged channel (both layouts; main.cpp disarms LEAN on it). An
    // unloaded amplifier reads ≈372 and the lowest seated reading in E9 was 369,
    // so 340 leaves room for offset spread while catching anything well below
    // zero force. Raised from 100 on 2026-11-22.
    static constexpr int SensorRailLow = 340, SensorRailHigh = 4000;

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
    lean::CentreOfPressure passive{}; // relaxed posture from a second press; invalid if none
    uint32_t neutralPressedAt = 0;
    bool passiveWindowOpen = false;   // a press now would record the relaxed posture
    lean::EmptyPlateTracker emptyTracker;
    lean::Counts lastReportedEmpty{};
    lean::Reach calibratedReach{};    // from a dynamic calibration, when reachCalibrated
    bool reachCalibrated = false;
    lean::Reach reachConfigAtCalibration{}; // configured reach when it was taken
    uint32_t seatEmptySince = 0;
    lean::CentreOfPressure lastDrivenCop{}; // CoP at the previous driving tick
    bool copJumped = false;
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
    void capturePassive();
    bool readyToDrive() const;
    bool calibrated() const;
    bool reachFromCalibration() const;
    bool cornerCalibrated() const;
    bool relaxedPressDue(uint32_t pressedAt) const;
    lean::Counts emptyPlate() const;
    lean::CentreOfPressure cornerCop(const lean::Counts &raw) const;

public:
    void reset() { initiallyCalibrated = false; passiveWindowOpen = false; cancelCalibration(); }
    MotorTargets getTargets() override;
    MotorTargets getTargetsFromSample();
    bool isSeated() const { return 2 * seatLoad > userWeightEstimate; }
    // True when the last sample had any raw reading at an ADC rail (shorted or open sensor).
    bool sensorAtRail() const { return railed; }
    // Corner layout: true when the last getTargetsFromSample() saw the centre of
    // pressure move further than lean::MaxCopStep since the previous driving tick.
    bool centreOfPressureJumped() const { return copJumped; }
    // Refresh the seat reading and optionally refresh lean telemetry.
    void sampleSensors(bool seatOnly = false);
    void calibrate();
    // A calibrate-button press. Corner layout: neutral, or the relaxed posture if
    // it comes within PassiveWindowMs of the neutral press (see the .cpp).
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
    // Once-a-second DBG CORNER line (neutral zone, reach boxes, baseline) on the
    // corner layout; nothing on the plus layout.
    void publishCornerStatus() const;
    // Call on every fresh 20 Hz sample. While disarmed with an empty seat, keeps
    // the corner empty-plate baseline zeroed in RAM (lean::EmptyPlateTracker).
    void trackEmptyPlate(uint32_t now, bool disarmed);
    const std::array<int, 4> &getRawReadings() const { return rawReadings; }
    const std::array<float, 4> &getDynamicGains() const { return dynamicGains; }
    void updateResponseCurve();
    // After any settings change (main.cpp applyRuntimeSettings): an edited reach
    // setting drops the hold-calibrated reach for good.
    void settingsChanged();
    const telemetry::SensorTelemetry &getTelemetry() const { return telemetry; }
    int getUserWeightEstimate() const { return userWeightEstimate; }

    static lean::SensorLayout layout();
    const lean::CentreOfPressure &getCentreOfPressure() const { return cop; }
    const lean::CentreOfPressure &getNeutral() const { return neutral; }
    const lean::CentreOfPressure &getPassive() const { return passive; }
    lean::NeutralZone neutralZone() const;
    lean::Counts emptyPlateInUse() const { return emptyPlate(); }
    // Dynamic-calibration reach if one was taken, otherwise the configured reach.
    lean::Reach activeReach() const;
    bool reachSavePending = false;
    bool reverseFlipped = false; // corner reverse-turn flip latch (see getTargetsFromSample)
    float turnKnobScale = 1.0f;
};
