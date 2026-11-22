// Compiles the actual LeanDriveMode with simulated ADC readings; no motors.
#include "lean_test_support.h"

static bool near(float a, float b, float tolerance = 0.002f) { return std::fabs(a - b) <= tolerance; }

// The response curve as documented: deadband, rescale, power.
static float shaped(float input, float deadband, const float knee[2])
{
    const float magnitude = std::fabs(input);
    if (magnitude <= deadband)
        return 0.0f;
    const float exponent = std::log(knee[1]) / std::log(knee[0]);
    return std::copysign(std::pow((magnitude - deadband) / (1 - deadband), exponent), input);
}
// The counts of lean that produce a given shaped output.
static int countsFor(float output, float deadband, const float knee[2], int range)
{
    const float exponent = std::log(knee[1]) / std::log(knee[0]);
    return static_cast<int>(std::lround((deadband + (1 - deadband) * std::pow(output, 1 / exponent)) * range));
}

int main()
{
    const float deadband = settings::deadband.get();
    const auto &speedKnee = settings::leanForewardBackResponse.get();
    const auto &turnKnee = settings::leanSideToSideResponse.get();
    const int fwd = settings::rangeForward.get(), back = settings::rangeBack.get();
    const int left = settings::rangeLeft.get(), right = settings::rangeRight.get();
    assert(fwd == 600 && back == 400 && left == 500 && right == 500);

    { // Seat: threshold, hysteresis and hold times, all in load counts.
        LeanRig rig;
        assert(settings::seatThreshold.get() == 200);
        loads(0, 65, 0, 0); // empty seat as logged: front at its zero, back resting 65 above
        rig.sample(1000);
        assert(!rig.mode.isSeated() && rig.mode.totalLoad() == 65);
        loads(50, 50, 50, 49); // 199: just under
        rig.sample(1000);
        assert(!rig.mode.isSeated());
        lean(0, 0); // a rider, 329 counts
        rig.sample(250);
        assert(!rig.mode.isSeated()); // not yet: must hold 0.3 s
        rig.sample(100);
        assert(rig.mode.isSeated() && rig.mode.totalLoad() == 329);
        loads(40, 40, 30, 30); // 140: under the threshold but above 60% of it
        rig.sample(1000);
        assert(rig.mode.isSeated());
        loads(10, 10, 10, 10); // a bump: briefly unloaded
        rig.sample(60);
        lean(0, 0);
        rig.sample(1000);
        assert(rig.mode.isSeated());
        loads(10, 10, 10, 10); // the rider is gone
        rig.sample(160);
        assert(!rig.mode.isSeated());
        // The threshold is a setting, not something learned from the last rider.
        lean(0, 0);
        rig.sample(400);
        assert(rig.mode.isSeated());
        assert(settings::seatThreshold.set(400));
        loads(0, 0, 0, 0);
        rig.sample(200);
        lean(0, 0);
        rig.sample(1000);
        assert(!rig.mode.isSeated());
        assert(settings::seatThreshold.set(200));
    }
    { // No drive until neutral is calibrated.
        LeanRig rig;
        lean(400, 300);
        auto command = rig.settle();
        assert(command.stopped && command.speed == 0 && command.turn == 0);
        assert(rig.mode.getTelemetry().stopped && rig.mode.getUserWeightEstimate() == 0);
        assert(rig.mode.calibrationLEDOn(0)); // on: needs calibration
    }

    LeanRig rig;
    lean(0, 0);
    rig.sample(500);
    rig.captureNeutral();
    assert(!rig.mode.calibrationLEDOn(rig.now));
    assert(rig.mode.getUserWeightEstimate() == NeutralF + NeutralB + NeutralL + NeutralR);
    auto command = rig.settle();
    assert(!command.stopped && command.speed == 0 && command.turn == 0);
    // Telemetry shows loads above each sensor's zero.
    assert(rig.mode.getTelemetry().front == NeutralF && rig.mode.getTelemetry().right == NeutralR);

    // Each direction scales by its own range.
    lean(300, 0);
    command = rig.settle();
    assert(near(command.speed, shaped(300.0f / fwd, deadband, speedKnee)) && command.turn == 0);
    assert(near(rig.mode.getTelemetry().speed, command.speed, 0) && !rig.mode.getTelemetry().stopped);
    lean(-200, 0);
    command = rig.settle();
    assert(near(command.speed, shaped(-200.0f / back, deadband, speedKnee)));
    lean(0, 250);
    command = rig.settle();
    assert(near(command.turn, shaped(250.0f / right, deadband, turnKnee)) && command.speed == 0);
    lean(0, -250);
    command = rig.settle();
    assert(near(command.turn, shaped(-250.0f / left, deadband, turnKnee)));
    // Full lean and beyond is full command, never more.
    lean(600, 500);
    command = rig.settle();
    assert(near(command.speed, 1) && near(command.turn, 1));
    lean(3000, -3000);
    command = rig.settle();
    assert(near(command.speed, 1) && near(command.turn, -1));

    // Load that lands on both sensors of a pair (a bump, shifting weight) is not a lean.
    lean(0, 0, 400);
    command = rig.settle();
    assert(command.speed == 0 && command.turn == 0);
    lean(300, 0, 400);
    command = rig.settle();
    assert(near(command.speed, shaped(300.0f / fwd, deadband, speedKnee)) && command.turn == 0);
    // A forward lean that also unloads the side sensors equally is not a turn
    // (dividing by total load would have made it one).
    loads(NeutralF + 600, 0, NeutralL - 20, NeutralR - 20);
    command = rig.settle();
    assert(command.turn == 0 && command.speed > 0.9f);

    // The response starts from zero at the deadband edge: no step.
    lean(static_cast<int>(deadband * fwd) - 1, 0);
    command = rig.settle();
    assert(command.speed == 0);
    lean(static_cast<int>(deadband * fwd) + 6, 0);
    command = rig.settle();
    assert(command.speed > 0 && command.speed < 0.005f);
    // The deadband never shrinks below a few counts on a very small range.
    assert(settings::rangeForward.set(40));
    lean(5, 0);
    command = rig.settle();
    assert(command.speed == 0); // 5 of 40 counts is 12%, past the 5% deadband, but under 6 counts
    lean(10, 0);
    command = rig.settle();
    assert(command.speed > 0);
    assert(settings::rangeForward.set(fwd));

    // Filtering: a step in the readings arrives within Spec K's 0.1 s.
    lean(0, 0);
    rig.settle();
    lean(600, 0);
    rig.sample(100);
    assert(rig.mode.command().speed > 0.9f);

    // Reversing: turn fades through zero around -0.3 and flips beyond it,
    // then fades out toward hard reverse. No step anywhere.
    const int sideCounts = 250;
    const float forwardTurn = shaped(250.0f / right, deadband, turnKnee);
    struct { float speed, factor; } points[] = {
        {0.20f, 1.0f}, {0.25f, 0.5f}, {0.30f, 0.0f}, {0.35f, -0.5f},
        {0.40f, -1.0f}, {0.60f, -1.0f}, {0.80f, -1.0f}, {0.85f, -0.5f}, {0.95f, 0.0f}};
    for (const auto &point : points)
    {
        lean(-countsFor(point.speed, deadband, speedKnee, back), sideCounts);
        command = rig.settle();
        assert(near(command.speed, -point.speed, 0.01f));
        // Tolerance: the speed lands within 0.01 of the wanted point and the factor has slope 10.
        assert(near(command.turn, forwardTurn * point.factor, forwardTurn * 0.12f));
        assert(near(rig.mode.getTelemetry().turn, command.turn, 0));
    }
    float previousTurn = 0;
    bool first = true;
    for (int counts = 0; counts <= back; counts += 2)
    { // sweep reverse slowly: the turn command never jumps
        lean(-counts, sideCounts);
        command = rig.settle(100);
        if (!first)
            assert(std::fabs(command.turn - previousTurn) < 0.08f);
        previousTurn = command.turn;
        first = false;
    }
    lean(200, sideCounts); // forward: untouched
    command = rig.settle();
    assert(near(command.turn, forwardTurn));

    // A reading at either ADC rail on any one sensor is a fault after 4 samples in a row.
    for (uint8_t pin : {common::PIN_F, common::PIN_B, common::PIN_L, common::PIN_R})
        for (int stuck : {4095, 0})
        {
            lean(0, 0);
            rig.sample();
            assert(!rig.mode.sensorAtRail());
            sensorInputs[pin] = stuck;
            rig.sample(15);
            assert(!rig.mode.sensorAtRail()); // 3 samples: a glitch
            rig.sample();
            assert(rig.mode.sensorAtRail());
            rig.sample(100);
            assert(rig.mode.sensorAtRail());
        }
    lean(0, 0);
    rig.sample();
    assert(!rig.mode.sensorAtRail()); // not latched here: the disarm is what sticks

    // reset() forgets neutral: no drive until the next calibration.
    lean(300, 0);
    rig.mode.reset();
    command = rig.settle();
    assert(command.stopped && !rig.mode.calibrated() && rig.mode.getUserWeightEstimate() == 0);
    for (uint16_t id : {10, 11, 12, 13, 20, 21, 22, 23, 36, 50, 51}) assert(!settings::registry().find(id));
    assert(!settings::registry().findGroup(3) && !settings::registry().findGroup(4));
    puts("Lean input, seat and sensor-fault tests passed");
}
