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

    // apply sensor specific gains
    b = (int)(b * settings::rearGain.get());
    f = (int)(f * settings::frontGain.get());
    l = (int)(l * settings::leftGain.get());
    r = (int)(r * settings::rightGain.get());

    telemetry = {f, b, l, r, 0.0f, 0.0f, !initiallyCalibrated};
}

MotorTargets LeanDriveMode::getTargets()
{
    sampleSensors();
    return getTargetsFromSample();
}

MotorTargets LeanDriveMode::getTargetsFromSample()
{
    digitalWrite(common::PIN_CAL_LED, initiallyCalibrated ? LOW : HIGH);
    if (!initiallyCalibrated)
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
