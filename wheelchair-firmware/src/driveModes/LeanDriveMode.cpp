#include "LeanDriveMode.h"
#include "Arduino.h"
#include "common.h"
#include "../config/DriveSettings.h"

static float normalizeSensorPair(int difference, int negativeLimit, int positiveLimit)
{
    if (difference >= 0)
    {
        return constrain(
            (float)difference / positiveLimit,
            0.0f,
            1.0f);
    }
    else
    {
        return constrain(
            (float)difference / negativeLimit,
            -1.0f,
            0.0f);
    }
}

static int aboveFloor(int raw, int floor) { return raw > floor ? raw - floor : 0; }

void LeanDriveMode::updateResponseCurve()
{
    const auto &speedKnee = settings::leanForewardBackResponse.get();
    speedResponseExponent = logf(speedKnee[1]) / logf(speedKnee[0]);
    const auto &turnKnee = settings::leanSideToSideResponse.get();
    turnResponseExponent = logf(turnKnee[1]) / logf(turnKnee[0]);
}

void LeanDriveMode::sampleSensors()
{
    // --- read, minus each sensor's floor ---
    const int defaultFloor = settings::defaultFloor.get();
    int f = aboveFloor(analogRead(common::PIN_F), initiallyCalibrated ? floorF : defaultFloor);
    int b = aboveFloor(analogRead(common::PIN_B), initiallyCalibrated ? floorB : defaultFloor);
    int l = aboveFloor(analogRead(common::PIN_L), initiallyCalibrated ? floorL : defaultFloor);
    int r = aboveFloor(analogRead(common::PIN_R), initiallyCalibrated ? floorR : defaultFloor);

    // apply sensor specific gains
    b = (int)(b * settings::rearGain.get());
    f = (int)(f * settings::frontGain.get());
    l = (int)(l * settings::leftGain.get());
    r = (int)(r * settings::rightGain.get());

    if (initiallyCalibrated)
    {
        f = min(frontSaturation, f);
        b = min(backSaturation, b);
        l = min(leftSaturation, l);
        r = min(rightSaturation, r);
    }
    telemetry = {f, b, l, r, 0.0f, 0.0f, !initiallyCalibrated};
}

MotorTargets LeanDriveMode::getTargets()
{
    sampleSensors();
    digitalWrite(common::PIN_CAL_LED, initiallyCalibrated ? LOW : HIGH);
    if (!initiallyCalibrated)
        return {0, 0};

    const int f = telemetry.front, b = telemetry.back;
    const int l = telemetry.left, r = telemetry.right;

    // Seat detection is currently disabled in the stop condition.

    // calculate speed and turn components from -1 to 1
    float speed = normalizeSensorPair(f - b, backSaturation, frontSaturation);
    float turn = normalizeSensorPair(r - l, leftSaturation, rightSaturation);

    // apply deadband.
    if (fabs(speed) < settings::deadband.get())
        speed = 0.0f;
    if (fabs(turn) < settings::deadband.get())
        turn = 0.0f;

    // Shape speed and turn separately before wheel mixing and PID smoothing.
    speed = copysignf(powf(fabsf(speed), speedResponseExponent), speed);
    turn = copysignf(powf(fabsf(turn), turnResponseExponent), turn);

    telemetry = {f, b, l, r, speed, turn, false};
    return {speed + turn, speed - turn};
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

    frontSaturation = settings::frontWeight.get() * userWeightEstimate;
    backSaturation = settings::rearWeight.get() * userWeightEstimate;
    leftSaturation = settings::leftWeight.get() * userWeightEstimate;
    rightSaturation = settings::rightWeight.get() * userWeightEstimate;

    initiallyCalibrated = true;

    // todo make telemetry handling more formal.
    Serial.printf("** Calibrated (%d samples): floor F=%d B=%d L=%d R=%d **\n",
                  samples, floorF, floorB, floorL, floorR);
    Serial1.printf("** Calibrated (%d samples): floor F=%d B=%d L=%d R=%d **\n",
                   samples, floorF, floorB, floorL, floorR);
}
