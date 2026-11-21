#include "LeanDriveMode.h"
#include "Arduino.h"
#include "common.h"
#include "../config/DriveSettings.h"
#include "../telemetry/Telemetry.h"

static float normalizeSensorPair(int difference, int userWeightEstimate)
{
    if (userWeightEstimate <= 0)
        return 0.0f;
    return constrain(static_cast<float>(difference) / userWeightEstimate, -1.0f, 1.0f);
}

static int aboveFloor(int raw, int floor) { return raw > floor ? raw - floor : 0; }

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
    const int defaultFloor = settings::defaultFloor.get();
    seatLoad = (rawF - defaultFloor) + (rawB - defaultFloor) +
               (rawL - defaultFloor) + (rawR - defaultFloor);
    if (seatOnly)
        return;

    int f = aboveFloor(rawF, initiallyCalibrated ? floorF : defaultFloor);
    int b = aboveFloor(rawB, initiallyCalibrated ? floorB : defaultFloor);
    int l = aboveFloor(rawL, initiallyCalibrated ? floorL : defaultFloor);
    int r = aboveFloor(rawR, initiallyCalibrated ? floorR : defaultFloor);

    flooredReadings = {{f, b, l, r}};
    f = static_cast<int>(f * dynamicGains[0]);
    b = static_cast<int>(b * dynamicGains[1]);
    l = static_cast<int>(l * dynamicGains[2]);
    r = static_cast<int>(r * dynamicGains[3]);

    telemetry = {f, b, l, r, 0.0f, 0.0f, !initiallyCalibrated || calibrationActive()};
}

MotorTargets LeanDriveMode::getTargets()
{
    sampleSensors();
    return getTargetsFromSample();
}

MotorTargets LeanDriveMode::getTargetsFromSample()
{
    if (!initiallyCalibrated || calibrationActive())
        return {0, 0};

    const int f = telemetry.front, b = telemetry.back;
    const int l = telemetry.left, r = telemetry.right;

    // calculate speed and turn components from -1 to 1
    float speed = normalizeSensorPair(f - b, userWeightEstimate);
    float turn = normalizeSensorPair(r - l, userWeightEstimate);

    // apply deadband.
    if (fabs(speed) < settings::deadband.get())
        speed = 0.0f;
    if (fabs(turn) < settings::deadband.get())
        turn = 0.0f;

    // Shape speed and turn separately before wheel mixing and PID smoothing.
    speed = copysignf(powf(fabsf(speed), speedResponseExponent), speed);
    turn = copysignf(powf(fabsf(turn), turnResponseExponent), turn);
    if (speed < -0.3f) {
        turn *= -1;
    }
    if (speed < -0.9f) {
        turn = 0;
    }


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

    telemetry::Telemetry::logf("** Calibrated (%d samples): floor F=%d B=%d L=%d R=%d **\n",
                               samples, floorF, floorB, floorL, floorR);
}

void LeanDriveMode::beginCalibration(uint32_t pressedAt)
{
    calibrate();
    calibrationPressedAt = pressedAt;
    calibrationPeaks.fill(0);
    calibrationState = CalibrationState::Waiting;
}

void LeanDriveMode::updateCalibration(uint32_t now, bool buttonHeld)
{
    if (!calibrationActive())
        return;
    if (!buttonHeld)
    {
        if (calibrationState == CalibrationState::Collecting)
            finishDynamicCalibration();
        calibrationState = CalibrationState::Idle;
        return;
    }
    if (calibrationState == CalibrationState::Waiting && now - calibrationPressedAt >= 300)
    {
        calibrationState = CalibrationState::Collecting;
        collectionStartedAt = now;
        telemetry::Telemetry::logf("Dynamic calibration started; lean in all four directions, then release.\n");
    }
    if (calibrationState == CalibrationState::Collecting)
        for (size_t i = 0; i < calibrationPeaks.size(); ++i)
            if (flooredReadings[i] > calibrationPeaks[i])
                calibrationPeaks[i] = flooredReadings[i];
}

void LeanDriveMode::finishDynamicCalibration()
{
    const float threshold = 0.1f * userWeightEstimate;
    const char *names[] = {"front", "back", "left", "right"};
    bool valid = userWeightEstimate > 0;
    if (!valid)
        telemetry::Telemetry::logf("Dynamic calibration rejected: weight estimate must be positive.\n");
    for (size_t i = 0; i < calibrationPeaks.size(); ++i)
    {
        if (calibrationPeaks[i] < threshold || calibrationPeaks[i] == 0)
        {
            valid = false;
            telemetry::Telemetry::logf("Dynamic calibration rejected: %s maximum %d, required %.1f.\n",
                                       names[i], calibrationPeaks[i], threshold);
        }
    }
    if (!valid)
        return;
    for (size_t i = 0; i < dynamicGains.size(); ++i)
        dynamicGains[i] = static_cast<float>(userWeightEstimate) / calibrationPeaks[i];
    telemetry::Telemetry::logf("Dynamic calibration complete: maxima F=%d B=%d L=%d R=%d.\n",
                               calibrationPeaks[0], calibrationPeaks[1], calibrationPeaks[2], calibrationPeaks[3]);
    publishDynamicGains();
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
    publishDynamicGains();
}

void LeanDriveMode::publishDynamicGains() const
{
    telemetry::Telemetry::logf("DYNAMIC_GAINS F=%.4f B=%.4f L=%.4f R=%.4f\n",
                               dynamicGains[0], dynamicGains[1], dynamicGains[2], dynamicGains[3]);
}
