#include "LeanDriveMode.h"
#include "Arduino.h"
#include "common.h"
#include "../config/DriveSettings.h"
#include "../telemetry/Telemetry.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

static int aboveFloor(int raw, int floor) { return raw - floor; }

// Raw 12-bit counts. An unloaded sensor reads about 372, so only a shorted or open sensor gets here.
constexpr int SENSOR_RAIL_LOW = 100, SENSOR_RAIL_HIGH = 4000;
static bool atRail(int raw) { return raw <= SENSOR_RAIL_LOW || raw >= SENSOR_RAIL_HIGH; }

lean::SensorLayout LeanDriveMode::layout() { return settings::sensorLayout.get(); }

static lean::Counts emptyPlate()
{
    return {{settings::emptyPlateF.get(), settings::emptyPlateB.get(),
             settings::emptyPlateL.get(), settings::emptyPlateR.get()}};
}

static lean::Gains cornerGains()
{
    return {{settings::cornerGainF.get(), settings::cornerGainB.get(),
             settings::cornerGainL.get(), settings::cornerGainR.get()}};
}

static lean::CentreOfPressure cornerCop(const lean::Counts &raw)
{
    return lean::centreOfPressure(lean::cornerForces(raw, emptyPlate(), cornerGains()));
}

bool LeanDriveMode::readyToDrive() const
{
    return initiallyCalibrated && !calibrationActive() && layout() == calibratedLayout;
}

lean::Reach LeanDriveMode::activeReach() const
{
    if (reachCalibrated)
        return calibratedReach;
    return {settings::reachForward.get(), settings::reachBack.get(),
            settings::reachLeft.get(), settings::reachRight.get()};
}

void LeanDriveMode::updateResponseCurve()
{
    const auto &speedKnee = settings::leanForewardBackResponse.get();
    speedResponseExponent = logf(speedKnee[1]) / logf(speedKnee[0]);
    const auto &turnKnee = settings::leanSideToSideResponse.get();
    turnResponseExponent = logf(turnKnee[1]) / logf(turnKnee[0]);
}

void LeanDriveMode::sampleSensors(bool seatOnly)
{
    const int rawF = analogRead(common::PIN_F);
    const int rawB = analogRead(common::PIN_B);
    const int rawL = analogRead(common::PIN_L);
    const int rawR = analogRead(common::PIN_R);
    railed = atRail(rawF) || atRail(rawB) || atRail(rawL) || atRail(rawR);
    rawReadings = {{rawF, rawB, rawL, rawR}};
    const int defaultFloor = settings::defaultFloor.get();
    seatLoad = (rawF - defaultFloor) + (rawB - defaultFloor) +
               (rawL - defaultFloor) + (rawR - defaultFloor);
    if (seatOnly)
        return;
    if (layout() == lean::SensorLayout::Corner)
    {
        sampleCorner();
        return;
    }
    cop = {};

    int f = aboveFloor(rawF, initiallyCalibrated ? floorF : defaultFloor);
    int b = aboveFloor(rawB, initiallyCalibrated ? floorB : defaultFloor);
    int l = aboveFloor(rawL, initiallyCalibrated ? floorL : defaultFloor);
    int r = aboveFloor(rawR, initiallyCalibrated ? floorR : defaultFloor);

    flooredReadings = {{f, b, l, r}};
    f = static_cast<int>(f * dynamicGains[0]);
    b = static_cast<int>(b * dynamicGains[1]);
    l = static_cast<int>(l * dynamicGains[2]);
    r = static_cast<int>(r * dynamicGains[3]);

    telemetry = {f, b, l, r, 0.0f, 0.0f, !readyToDrive()};
}

void LeanDriveMode::sampleCorner()
{
    const auto forces = lean::cornerForces(rawReadings, emptyPlate(), cornerGains());
    cop = lean::centreOfPressure(forces);
    // The telemetry F/B/L/R columns carry each channel's gain-scaled force above
    // the empty plate, the same numbers the centre of pressure is built from.
    telemetry = {static_cast<int>(forces[lean::F] + 0.5f), static_cast<int>(forces[lean::B] + 0.5f),
                 static_cast<int>(forces[lean::L] + 0.5f), static_cast<int>(forces[lean::R] + 0.5f),
                 0.0f, 0.0f, !readyToDrive()};
}

MotorTargets LeanDriveMode::getTargets()
{
    sampleSensors();
    return getTargetsFromSample();
}

MotorTargets LeanDriveMode::getTargetsFromSample()
{
    if (!readyToDrive())
        return {0, 0};

    const int f = telemetry.front, b = telemetry.back;
    const int l = telemetry.left, r = telemetry.right;

    // Each layout produces speed and turn from -1 to 1; shapeCommand then applies
    // the shared deadband, response curves and reverse-turn flip.
    lean::Command command;
    if (calibratedLayout == lean::SensorLayout::Corner)
    {
        if (!cop.valid || !neutral.valid)
        {
            telemetry.speed = telemetry.turn = 0.0f;
            telemetry.stopped = true;
            return {0, 0};
        }
        // The corner deadzone is in inches inside cornerCommand, with the output
        // ramping from its edge, so the normalised deadband is skipped (0).
        command = lean::shapeCommand(
            lean::cornerCommand(cop.at, neutral.at, activeReach(), settings::cornerDeadzone.get()),
            0.0f, speedResponseExponent, turnResponseExponent);
    }
    else
    {
        command = lean::shapeCommand(lean::plusCommand(f - b, r - l, userWeightEstimate),
                                     settings::deadband.get(), speedResponseExponent, turnResponseExponent);
    }
    const float speed = command.speed, turn = command.turn;

    telemetry = {f, b, l, r, speed, turn, false};
    const auto &mix = settings::leanTurnMix.get();
    const float speedMagnitude = fabsf(speed);
    const int segment = speedMagnitude < mix[2] ? 0 : 2;
    const float ratio = (speedMagnitude - mix[segment]) / (mix[segment + 2] - mix[segment]);
    const float turnGain = mix[segment + 1] + ratio * (mix[segment + 3] - mix[segment + 1]);
    const float mixedTurn = turnGain * turn;
    return {constrain(speed + mixedTurn, -1.0f, 1.0f),
            constrain(speed - mixedTurn, -1.0f, 1.0f)};
}

void LeanDriveMode::calibrate()
{

    const int samples = settings::calibrationSamples.get();
    long sumF = 0, sumB = 0, sumL = 0, sumR = 0;
    for (int i = 0; i < samples; i++)
    {
        sumF += analogRead(common::PIN_F);
        sumB += analogRead(common::PIN_B);
        sumL += analogRead(common::PIN_L);
        sumR += analogRead(common::PIN_R);
        delay(2);
    }
    floorF = (sumF + samples / 2) / samples;
    floorB = (sumB + samples / 2) / samples;
    floorL = (sumL + samples / 2) / samples;
    floorR = (sumR + samples / 2) / samples;
    userWeightEstimate = (floorF + floorB + floorL + floorR) / 4; // integer division ok here.

    initiallyCalibrated = true;
    calibratedLayout = layout();

    telemetry::Telemetry::logf("** Calibrated (%d samples): floor F=%d B=%d L=%d R=%d **\n",
                               samples, floorF, floorB, floorL, floorR);
    neutral = {};
    if (calibratedLayout != lean::SensorLayout::Corner)
        return;
    // The same averaged readings set the rider's neutral centre of pressure. The
    // rider should sit upright and engaged (active neutral) when pressing.
    neutral = cornerCop({{floorF, floorB, floorL, floorR}});
    if (neutral.valid)
        telemetry::Telemetry::logf("** Neutral centre of pressure x=%.2f y=%.2f in (load %.0f) **\n",
                                   neutral.at.x, neutral.at.y, neutral.total);
    else
        telemetry::Telemetry::logf("Calibration: plate load %.0f is too low for a centre of pressure; LEAN stays stopped.\n",
                                   neutral.total);
}

void LeanDriveMode::beginCalibration(uint32_t pressedAt)
{
    calibrate();
    calibrationPressedAt = pressedAt;
    calibrationPeaks.fill(0);
    oppositeAtPeak.fill(0);
    reachPeaks = {};
    calibrationState = CalibrationState::Waiting;
}

bool LeanDriveMode::updateCalibration(uint32_t now, bool buttonHeld)
{
    if (!calibrationActive())
        return false;
    if (!buttonHeld)
    {
        const bool dynamicFinished = calibrationState == CalibrationState::Collecting;
        if (dynamicFinished)
            finishDynamicCalibration();
        calibrationState = CalibrationState::Idle;
        return dynamicFinished;
    }
    if (calibrationState == CalibrationState::Waiting && now - calibrationPressedAt >= 300)
    {
        calibrationState = CalibrationState::Collecting;
        collectionStartedAt = now;
        telemetry::Telemetry::logf("Dynamic calibration started; lean in all four directions, then release.\n");
    }
    if (calibrationState == CalibrationState::Collecting && calibratedLayout == lean::SensorLayout::Corner)
    {
        // Furthest centre-of-pressure travel from neutral in each direction.
        if (cop.valid && neutral.valid)
        {
            const float dx = cop.at.x - neutral.at.x, dy = cop.at.y - neutral.at.y;
            reachPeaks.forward = std::max(reachPeaks.forward, dy);
            reachPeaks.back = std::max(reachPeaks.back, -dy);
            reachPeaks.right = std::max(reachPeaks.right, dx);
            reachPeaks.left = std::max(reachPeaks.left, -dx);
        }
    }
    else if (calibrationState == CalibrationState::Collecting)
    {
        const size_t opposite[] = {1, 0, 3, 2};
        for (size_t i = 0; i < calibrationPeaks.size(); ++i)
            if (flooredReadings[i] > calibrationPeaks[i])
            {
                calibrationPeaks[i] = flooredReadings[i];
                oppositeAtPeak[i] = flooredReadings[opposite[i]];
            }
    }
    return false;
}

void LeanDriveMode::finishDynamicCalibration()
{
    if (calibratedLayout == lean::SensorLayout::Corner)
    {
        finishReachCalibration();
        return;
    }
    const float threshold = 0.1f * userWeightEstimate;
    const char *names[] = {"front", "back", "left", "right"};
    std::array<int, 4> differences{};
    bool valid = userWeightEstimate > 0;
    if (!valid)
        telemetry::Telemetry::logf("Dynamic calibration rejected: weight estimate must be positive.\n");
    for (size_t i = 0; i < calibrationPeaks.size(); ++i)
    {
        differences[i] = calibrationPeaks[i] - oppositeAtPeak[i];
        if (calibrationPeaks[i] < threshold || calibrationPeaks[i] == 0)
        {
            valid = false;
            telemetry::Telemetry::logf("Dynamic calibration rejected: %s maximum %d, required %.1f.\n",
                                       names[i], calibrationPeaks[i], threshold);
        }
        if (differences[i] <= 0)
        {
            valid = false;
            telemetry::Telemetry::logf("Dynamic calibration rejected: %s difference at peak %d must be positive.\n",
                                       names[i], differences[i]);
        }
    }
    if (!valid)
        return;
    for (size_t i = 0; i < dynamicGains.size(); ++i)
        dynamicGains[i] = static_cast<float>(userWeightEstimate) / differences[i];
    telemetry::Telemetry::logf("Dynamic calibration complete: differences at peaks F=%d B=%d L=%d R=%d.\n",
                               differences[0], differences[1], differences[2], differences[3]);
    publishDynamicGains();
}

// Corner counterpart of the plus gains: each direction's furthest travel becomes
// that direction's full lean. As with the plus layout, one weak direction rejects
// all four and the previous reach stays.
void LeanDriveMode::finishReachCalibration()
{
    const float peaks[] = {reachPeaks.forward, reachPeaks.back, reachPeaks.left, reachPeaks.right};
    const char *names[] = {"forward", "back", "left", "right"};
    bool valid = neutral.valid;
    if (!valid)
        telemetry::Telemetry::logf("Reach calibration rejected: no neutral centre of pressure.\n");
    for (size_t i = 0; i < 4; ++i)
        if (peaks[i] < lean::MinReach)
        {
            valid = false;
            telemetry::Telemetry::logf("Reach calibration rejected: %s reached %.2f in, required %.2f.\n",
                                       names[i], peaks[i], lean::MinReach);
        }
    if (!valid)
        return;
    calibratedReach = {std::min(peaks[0], lean::MaxReach), std::min(peaks[1], lean::MaxReach),
                       std::min(peaks[2], lean::MaxReach), std::min(peaks[3], lean::MaxReach)};
    reachCalibrated = true;
    telemetry::Telemetry::logf("Reach calibration complete: forward %.2f back %.2f left %.2f right %.2f in.\n",
                               calibratedReach.forward, calibratedReach.back, calibratedReach.left, calibratedReach.right);
}

void LeanDriveMode::cancelCalibration()
{
    if (calibrationActive())
        telemetry::Telemetry::logf("Dynamic calibration cancelled; keeping active gains.\n");
    calibrationState = CalibrationState::Idle;
}

bool LeanDriveMode::calibrationLEDOn(uint32_t now) const
{
    if (calibrationState == CalibrationState::Collecting)
        return ((now - collectionStartedAt) / 100) % 2 == 0;
    return !initiallyCalibrated;
}

void LeanDriveMode::resetDynamicGains()
{
    dynamicGains = {{settings::frontGain.get(), settings::rearGain.get(),
                     settings::leftGain.get(), settings::rightGain.get()}};
    reachCalibrated = false;
    publishDynamicGains();
}

void LeanDriveMode::publishDynamicGains() const
{
    // The dashboard shows these next to each channel as its active gain.
    const auto gains = layout() == lean::SensorLayout::Corner ? cornerGains() : dynamicGains;
    telemetry::Telemetry::logf("DYNAMIC_GAINS F=%.4f B=%.4f L=%.4f R=%.4f\n",
                               gains[0], gains[1], gains[2], gains[3]);
}

void LeanDriveMode::publishSensorStatus() const
{
    // Corner fields: last CoP, calibrated neutral and the reach in use
    // (forward/back/left/right), all in inches. Keep the line under Telemetry's
    // 256-byte limit (≈210 at most today).
    char corner[112] = "";
    if (layout() == lean::SensorLayout::Corner)
    {
        const auto reach = activeReach();
        snprintf(corner, sizeof(corner), " cop=%.2f/%.2f neutral=%.2f/%.2f reach=%.2f/%.2f/%.2f/%.2f%s",
                 cop.at.x, cop.at.y, neutral.at.x, neutral.at.y,
                 reach.forward, reach.back, reach.left, reach.right, reachCalibrated ? "*" : "");
    }
    telemetry::Telemetry::logf("DBG SENSORS raw=%d/%d/%d/%d floors=%d/%d/%d/%d W=%d seatLoad=%d seated=%d calibrated=%d calActive=%d railed=%d layout=%s%s\n",
                               rawReadings[0], rawReadings[1], rawReadings[2], rawReadings[3],
                               floorF, floorB, floorL, floorR, userWeightEstimate, seatLoad,
                               isSeated() ? 1 : 0, initiallyCalibrated ? 1 : 0, calibrationActive() ? 1 : 0, railed ? 1 : 0,
                               lean::layoutName(layout()), corner);
}

void LeanDriveMode::publishCentreOfPressure(uint32_t now) const
{
    if (layout() != lean::SensorLayout::Corner)
        return;
    // ms, CoP x and y (inches, plate coordinates), offset from the calibrated
    // neutral (nan until calibrated), total load (gain-scaled counts above the
    // empty plate). x and y are nan when the load is too small to locate.
    const bool located = cop.valid;
    const bool offset = located && neutral.valid && initiallyCalibrated && calibratedLayout == lean::SensorLayout::Corner;
    telemetry::Telemetry::logf("DBG COP %lu %.2f %.2f %.2f %.2f %.0f\n", static_cast<unsigned long>(now),
                               located ? cop.at.x : NAN, located ? cop.at.y : NAN,
                               offset ? cop.at.x - neutral.at.x : NAN, offset ? cop.at.y - neutral.at.y : NAN,
                               cop.total);
}
