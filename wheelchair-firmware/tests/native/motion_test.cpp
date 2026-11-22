// Motion smoothing on two axes and the stop sequence; pure logic, no hardware.
#include "motion/MotionSmoother.h"
#include "motion/StopController.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace DriveConstants;

constexpr float dt = 0.005f;
static bool near(float a, float b, float tolerance = 0.01f) { return std::fabs(a - b) <= tolerance; }

static void smootherTests()
{
    const float turnAccel = 300.0f, turnDecel = 450.0f; // wheel RPM per second
    { // Steering is there while the chair is still accelerating.
        MotionSmoother motion(RPMSetpointPID::defaultTuning());
        MotionSmoother::Wheels wheels{};
        for (int k = 0; k < 40; ++k) // 0.2 s
            wheels = motion.update(150.0f, 20.0f, dt, MAX_SPEED, turnAccel, turnDecel);
        const float mean = (wheels.left + wheels.right) / 2.0f;
        assert(mean > 5.0f && mean < 100.0f);                 // well short of the 150 RPM target
        assert(near((wheels.left - wheels.right) / 2.0f, 20.0f)); // but the full turn is already there
        // The per-wheel smoother this replaces gave both wheels the same
        // limited acceleration, so their difference stayed near zero here.
    }
    { // Turn builds at the accel rate and dies at the (faster) decel rate.
        MotionSmoother motion(RPMSetpointPID::defaultTuning());
        for (int k = 0; k < 20; ++k) // 0.1 s
            motion.update(0.0f, 100.0f, dt, MAX_SPEED, turnAccel, turnDecel);
        assert(near(motion.turnRPM(), 30.0f, 0.05f));
        int steps = 0;
        while (motion.turnRPM() > 0.0f && steps < 1000)
        {
            motion.update(0.0f, 0.0f, dt, MAX_SPEED, turnAccel, turnDecel);
            ++steps;
        }
        assert(steps == 14); // 30 RPM / 450 RPM/s = 67 ms, not the 100 ms it took to build
        assert(motion.turnRPM() == 0.0f); // lands on zero, no overshoot
        // Reversing the turn sheds the old rotation at the decel rate first.
        MotionSmoother flip(RPMSetpointPID::defaultTuning());
        flip.seed(30.0f, -30.0f);
        flip.update(0.0f, -100.0f, dt, MAX_SPEED, turnAccel, turnDecel);
        assert(near(flip.turnRPM(), 30.0f - turnDecel * dt, 0.001f));
    }
    { // At the speed limit the chair slows down to keep the turn it was asked for.
        MotionSmoother motion(RPMSetpointPID::defaultTuning());
        const float limitMS = 1.0f, maxRPM = limitMS * LINEAR_TO_RPM;
        MotionSmoother::Wheels wheels{};
        for (int k = 0; k < 2000; ++k)
            wheels = motion.update(maxRPM, 10.0f, dt, limitMS, turnAccel, turnDecel);
        assert(near(wheels.left, maxRPM, 0.01f));
        assert(near(wheels.left - wheels.right, 20.0f, 0.01f));
        assert(std::fabs(wheels.left) <= maxRPM + 0.001f && std::fabs(wheels.right) <= maxRPM + 0.001f);
        // A turn request beyond the limit is cut to the limit, never past it.
        for (int k = 0; k < 2000; ++k)
            wheels = motion.update(0.0f, 10.0f * maxRPM, dt, limitMS, turnAccel, turnDecel);
        assert(near(wheels.left, maxRPM, 0.01f) && near(wheels.right, -maxRPM, 0.01f));
    }
    { // Engaging on a rolling chair starts from the wheels' speed, not from zero.
        MotionSmoother motion(RPMSetpointPID::defaultTuning());
        motion.seed(62.0f, 58.0f);
        assert(near(motion.linearRPM(), 60.0f) && near(motion.turnRPM(), 2.0f));
        const auto wheels = motion.update(60.0f, 2.0f, dt, MAX_SPEED, turnAccel, turnDecel);
        assert(near(wheels.left, 62.0f, 0.5f) && near(wheels.right, 58.0f, 0.5f));
        // Asked to stop, the command comes down from there at no more than MAX_ACCEL.
        float previous = motion.linearRPM();
        for (int k = 0; k < 400; ++k)
        {
            motion.update(0.0f, 0.0f, dt, MAX_SPEED, turnAccel, turnDecel);
            // Falling all the way down (the tuned smoother may settle around zero).
            if (previous > 2.0f)
                assert(motion.linearRPM() <= previous + 0.0001f);
            assert(std::fabs(previous - motion.linearRPM()) <= MAX_ACCEL * LINEAR_TO_RPM * dt + 0.001f);
            previous = motion.linearRPM();
        }
        assert(std::fabs(previous) < 1.0f);
    }
    { // unwind(): a command that ran ahead of the wheels drops to their speed
      // when the rider asks for less than the chair is doing; otherwise it is left alone.
        MotionSmoother motion(RPMSetpointPID::defaultTuning());
        motion.seed(110.0f, 90.0f);              // command: 100 forward, 10 turn
        motion.unwind(60.0f, 4.0f, 150.0f, 20.0f); // wheels lag, rider still wants more: no change
        assert(near(motion.linearRPM(), 100.0f) && near(motion.turnRPM(), 10.0f));
        motion.unwind(60.0f, 4.0f, 0.0f, 20.0f);   // forward released: that axis only
        assert(near(motion.linearRPM(), 60.0f) && near(motion.turnRPM(), 10.0f));
        motion.unwind(60.0f, 4.0f, 0.0f, 0.0f);    // turn released too
        assert(near(motion.turnRPM(), 4.0f));
        // Inside the margin (ordinary tracking error) nothing happens.
        motion.seed(64.0f, 56.0f);
        motion.unwind(60.0f - MotionSmoother::LinearUnwindRPM + 5.5f, 4.0f - MotionSmoother::TurnUnwindRPM + 2.5f, 0.0f, 0.0f);
        assert(near(motion.linearRPM(), 60.0f) && near(motion.turnRPM(), 4.0f));
        // Mirror: the command fell below the wheels and the rider asks for more.
        motion.seed(10.0f, 10.0f);
        motion.unwind(40.0f, 0.0f, 80.0f, 0.0f);
        assert(near(motion.linearRPM(), 40.0f));
        // Reverse, and a target between command and wheels (no reason to jump).
        motion.seed(-100.0f, -100.0f);
        motion.unwind(-50.0f, 0.0f, 0.0f, 0.0f);
        assert(near(motion.linearRPM(), -50.0f));
        motion.seed(100.0f, 100.0f);
        motion.unwind(60.0f, 0.0f, 80.0f, 0.0f);
        assert(near(motion.linearRPM(), 100.0f));
        motion.unwind(NAN, 0.0f, 0.0f, 0.0f);
        assert(near(motion.linearRPM(), 100.0f));
    }
    { // Bad numbers stop the chair instead of reaching the wheels.
        MotionSmoother motion(RPMSetpointPID::defaultTuning());
        motion.seed(50.0f, 50.0f);
        auto wheels = motion.update(NAN, 0.0f, dt, MAX_SPEED, turnAccel, turnDecel);
        assert(wheels.left == 0.0f && wheels.right == 0.0f && motion.linearRPM() == 0.0f);
        motion.seed(50.0f, 50.0f);
        wheels = motion.update(10.0f, INFINITY, dt, MAX_SPEED, turnAccel, turnDecel);
        assert(wheels.left == 0.0f && wheels.right == 0.0f);
        wheels = motion.update(10.0f, 0.0f, dt, 0.0f, turnAccel, turnDecel);
        assert(wheels.left == 0.0f && wheels.right == 0.0f);
        motion.seed(NAN, 1.0f);
        assert(motion.linearRPM() == 0.0f && motion.turnRPM() == 0.0f);
    }
}

// Runs a stop against a wheel that follows the command (or not), returns seconds until release.
struct StopRun
{
    float seconds = 0, peakBrakeSeconds = 0;
    bool sawRamp = false, sawBrake = false, monotonic = true, ratioKept = true;
    float lastLeft = 0, lastRight = 0;
};

static StopRun runStop(float left0, float right0, bool feedback, float followRPMPerS)
{
    StopController stop;
    StopRun run;
    uint32_t now = 1000;
    float left = left0, right = right0; // the wheels
    stop.begin(left0, right0, now);
    float previous = std::fmax(std::fabs(left0), std::fabs(right0));
    for (int k = 0; k < 4000 && stop.active(); ++k)
    {
        now += 5;
        const auto out = stop.update(dt, now, {left, right, feedback});
        if (out.drive == StopController::Drive::RPM)
        {
            run.sawRamp = true;
            assert(!run.sawBrake); // never back from brake to ramp
            const float peak = std::fmax(std::fabs(out.left), std::fabs(out.right));
            run.monotonic = run.monotonic && peak <= previous + 0.0001f;
            // Each command moves toward zero and never changes sign.
            assert(out.left * left0 >= 0.0f && out.right * right0 >= 0.0f);
            if (std::fabs(left0) > 1.0f && std::fabs(right0) > 1.0f)
                run.ratioKept = run.ratioKept && near(out.left / out.right, left0 / right0, 0.001f);
            previous = peak;
            const float step = followRPMPerS * dt;
            left += std::fmin(step, std::fmax(-step, out.left - left));
            right += std::fmin(step, std::fmax(-step, out.right - right));
        }
        else if (out.drive == StopController::Drive::Brake)
        {
            run.sawBrake = true;
            assert(out.left == STOP_BRAKE_AMPS && out.right == STOP_BRAKE_AMPS);
            run.peakBrakeSeconds += dt;
            const float step = 100.0f * dt; // brake current slows the wheel
            left -= std::copysign(std::fmin(step, std::fabs(left)), left);
            right -= std::copysign(std::fmin(step, std::fabs(right)), right);
        }
        run.seconds += dt;
    }
    assert(!stop.active());
    const auto after = stop.update(dt, now + 5, {left, right, feedback});
    assert(after.drive == StopController::Drive::Release);
    run.lastLeft = left;
    run.lastRight = right;
    return run;
}

static void stopTests()
{
    const float decelRPMS = STOP_DECEL * LINEAR_TO_RPM;
    { // From each speed-knob position: ramp, brake, release, inside the documented time.
        const float fractions[] = {0.25f, 0.5f, 0.75f, 1.0f};
        const float documented[] = {0.33f, 0.66f, 1.0f, 1.33f}; // drive_constants.h
        for (int i = 0; i < 4; ++i)
        {
            const float rpm = fractions[i] * MAX_SPEED * LINEAR_TO_RPM;
            const auto run = runStop(rpm, rpm, true, 400.0f);
            assert(run.sawRamp && run.sawBrake && run.monotonic);
            const float rampSeconds = (rpm - STOP_HANDOFF_RPM) / decelRPMS;
            assert(rampSeconds <= documented[i]);
            // Ramp, then the short brake phase from the handoff speed, then settle.
            assert(run.seconds >= rampSeconds && run.seconds <= rampSeconds + 0.5f);
            assert(std::fabs(run.lastLeft) < STOP_DONE_RPM && std::fabs(run.lastRight) < STOP_DONE_RPM);
        }
    }
    { // The command starts falling on the first tick, at exactly STOP_DECEL.
        StopController stop;
        stop.begin(100.0f, 100.0f, 0);
        const auto first = stop.update(dt, 5, {100.0f, 100.0f, true});
        assert(first.drive == StopController::Drive::RPM);
        assert(near(first.left, 100.0f - decelRPMS * dt, 0.001f));
        assert(near(stop.leftRPM(), first.left, 0.0001f)); // what a resumed drive would start from
    }
    { // A turning chair stops along its curve: both wheels reach zero together.
        const auto run = runStop(120.0f, 40.0f, true, 400.0f);
        assert(run.ratioKept && run.monotonic && run.sawBrake);
        const auto spin = runStop(60.0f, -60.0f, true, 400.0f);
        assert(spin.ratioKept && spin.sawBrake);
        const auto reverse = runStop(-80.0f, -80.0f, true, 400.0f);
        assert(reverse.monotonic && reverse.sawBrake);
    }
    { // No feedback (a silent VESC): same ramp, then the brake runs its full time.
        const auto run = runStop(100.0f, 100.0f, false, 400.0f);
        assert(run.sawRamp && run.sawBrake);
        assert(near(run.peakBrakeSeconds, STOP_BRAKE_MAX_MS / 1000.0f, 0.011f));
        // Speed unknown: brake current only, for the full time.
        StopController blind;
        blind.beginBlind(0);
        assert(blind.phase() == StopController::Phase::Brake);
        assert(blind.update(dt, 5, {0, 0, false}).drive == StopController::Drive::Brake);
        assert(blind.update(dt, STOP_BRAKE_MAX_MS - 1, {0, 0, false}).drive == StopController::Drive::Brake);
        assert(blind.update(dt, STOP_BRAKE_MAX_MS, {0, 0, false}).drive == StopController::Drive::Release);
    }
    { // Already slow: straight to brake current, released once the wheels settle.
        StopController stop;
        stop.begin(10.0f, -8.0f, 0);
        assert(stop.phase() == StopController::Phase::Brake);
        assert(stop.update(dt, 5, {10.0f, -8.0f, true}).drive == StopController::Drive::Brake);
        assert(stop.update(dt, 10, {1.0f, -1.0f, true}).drive == StopController::Drive::Brake);
        assert(stop.update(dt, 10 + STOP_SETTLE_MS - 1, {1.0f, 0.0f, true}).drive == StopController::Drive::Brake);
        // A wheel that speeds up again restarts the settle time.
        assert(stop.update(dt, 200, {5.0f, 0.0f, true}).drive == StopController::Drive::Brake);
        assert(stop.update(dt, 205, {1.0f, 0.0f, true}).drive == StopController::Drive::Brake);
        assert(stop.update(dt, 205 + STOP_SETTLE_MS, {1.0f, 0.0f, true}).drive == StopController::Drive::Release);
        assert(!stop.active());
    }
    { // Wheels that lag the ramp: hold the handoff speed, bounded, then brake.
        StopController stop;
        uint32_t now = 0;
        stop.begin(100.0f, 100.0f, now);
        StopController::Output out{};
        uint32_t rampEnd = 0;
        while (true)
        {
            now += 5;
            out = stop.update(dt, now, {90.0f, 90.0f, true}); // the chair is not slowing
            assert(out.drive == StopController::Drive::RPM);
            if (near(out.left, STOP_HANDOFF_RPM, 0.001f))
            {
                rampEnd = now;
                break;
            }
            assert(now < 2000);
        }
        // Still an RPM command at the handoff speed just before the overrun limit...
        out = stop.update(dt, rampEnd + STOP_RAMP_OVERRUN_MS - 5, {90.0f, 90.0f, true});
        assert(out.drive == StopController::Drive::RPM && near(out.left, STOP_HANDOFF_RPM, 0.001f));
        // ...and brake current from the limit on.
        out = stop.update(dt, rampEnd + STOP_RAMP_OVERRUN_MS, {90.0f, 90.0f, true});
        assert(out.drive == StopController::Drive::Brake);
        // Wheels that did come down hand off at once.
        StopController quick;
        quick.begin(STOP_HANDOFF_RPM + 1.0f, STOP_HANDOFF_RPM + 1.0f, 0);
        out = quick.update(dt, 5, {20.0f, 20.0f, true});
        assert(out.drive == StopController::Drive::RPM); // one tick of ramp left
        out = quick.update(dt, 10, {20.0f, 20.0f, true});
        assert(out.drive == StopController::Drive::Brake);
    }
    { // The brake phase never runs past its limit, even if the wheels never stop.
        StopController stop;
        stop.begin(5.0f, 5.0f, 0);
        assert(stop.update(dt, STOP_BRAKE_MAX_MS - 5, {50.0f, 50.0f, true}).drive == StopController::Drive::Brake);
        assert(stop.update(dt, STOP_BRAKE_MAX_MS, {50.0f, 50.0f, true}).drive == StopController::Drive::Release);
    }
    { // cancel() and non-finite speeds.
        StopController stop;
        stop.begin(100.0f, 100.0f, 0);
        stop.cancel();
        assert(!stop.active() && stop.leftRPM() == 0.0f);
        stop.begin(NAN, INFINITY, 0);
        assert(stop.phase() == StopController::Phase::Brake); // treated as unknown speed
    }
}

int main()
{
    smootherTests();
    stopTests();
    puts("Motion smoother and stop controller tests passed");
}
