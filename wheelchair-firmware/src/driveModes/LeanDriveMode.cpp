#include "LeanDriveMode.h"
#include "Arduino.h"
#include "common.h"

float normalizeSensorPair(int difference, int negativeLimit, int positiveLimit)
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

int aboveFloor(int raw, int floor) { return raw > floor ? raw - floor : 0; }

// need to refactor this to be configuration compatible.
//  Calculate once; configurable knee coordinates live in drive_constants.h.
const float INPUT_RESPONSE_EXPONENT =
    logf(common::INPUT_RESPONSE_KNEE_OUTPUT) / logf(common::INPUT_RESPONSE_KNEE_INPUT);
/** Map a normalized [-1, 1] lean input through the configured power curve.
 *  Preserves direction, zero and full scale; call after the input deadband.
 */
float applyLeanResponseCurve(float input)
{
    return copysignf(powf(fabsf(input), INPUT_RESPONSE_EXPONENT), input);
}

MotorTargets LeanDriveMode::getTargets()
{
    int f = 0, b = 0, l = 0, r = 0, total = 0;
    float speed = 0.0f, turn = 0.0f;

    // force calibration before the wheels can move.
    if (!initiallyCalibrated)
    {
        stopped = true;
        return {0, 0};
    }

    // --- read, minus each sensor's floor ---
    f = aboveFloor(analogRead(common::PIN_F), floorF);
    b = aboveFloor(analogRead(common::PIN_B), floorB);
    l = aboveFloor(analogRead(common::PIN_L), floorL);
    r = aboveFloor(analogRead(common::PIN_R), floorR);

    // apply sensor specific gains
    b = (int)(b * common::REAR_GAIN);
    f = (int)(f * common::FRONT_GAIN);
    l = (int)(l * common::LEFT_GAIN);
    r = (int)(r * common::RIGHT_GAIN);

    // apply saturations
    f = min(frontSaturation, f);
    b = min(frontSaturation, b);
    l = min(frontSaturation, l);
    r = min(frontSaturation, r);

    total = f + b + l + r;
    // Seat detection is currently disabled in the stop condition.

    // calculate speed and turn components from -1 to 1
    speed = normalizeSensorPair(f - b, backSaturation, frontSaturation);
    turn = normalizeSensorPair(r - l, leftSaturation, rightSaturation);

    // apply deadband.
    if (fabs(speed) < common::DEADBAND)
        speed = 0.0f;
    if (fabs(turn) < common::DEADBAND)
        turn = 0.0f;

    // Shape speed and turn separately before wheel mixing and PID smoothing.
    speed = applyLeanResponseCurve(speed);
    turn = applyLeanResponseCurve(turn);

    return {speed + turn, speed - turn};
}

void LeanDriveMode::calibrate()
{

    long sumF = 0, sumB = 0, sumL = 0, sumR = 0;
    for (int i = 0; i < common::CAL_SAMPLES; i++)
    {
        sumF += analogRead(common::PIN_F);
        sumB += analogRead(common::PIN_B);
        sumL += analogRead(common::PIN_L);
        sumR += analogRead(common::PIN_R);
        delay(2);
    }
    floorF = (sumF + common::CAL_SAMPLES / 2) / common::CAL_SAMPLES;
    floorB = (sumB + common::CAL_SAMPLES / 2) / common::CAL_SAMPLES;
    floorL = (sumL + common::CAL_SAMPLES / 2) / common::CAL_SAMPLES;
    floorR = (sumR + common::CAL_SAMPLES / 2) / common::CAL_SAMPLES;
    userWeightEstimate = (floorF + floorB + floorL + floorR) / 4; // integer division ok here.

    frontSaturation = frontWeightFactor * userWeightEstimate;
    backSaturation = backWeightFactor * userWeightEstimate;
    leftSaturation = leftWeightFactor * userWeightEstimate;
    rightSaturation = rightWeightFactor * userWeightEstimate;

    // clear calibration gate at start of program.
    if (!initiallyCalibrated)
    {
        initiallyCalibrated = true;
        stopped = false;
    }

    // todo make telemetry handling more formal.
    Serial.printf("** Calibrated (%d samples): floor F=%d B=%d L=%d R=%d **\n",
                  common::CAL_SAMPLES, floorF, floorB, floorL, floorR);
    Serial1.printf("** Calibrated (%d samples): floor F=%d B=%d L=%d R=%d **\n",
                   common::CAL_SAMPLES, floorF, floorB, floorL, floorR);
}
