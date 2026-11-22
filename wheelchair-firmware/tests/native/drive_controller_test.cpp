// Whole rides on the host: the real DriveController (mode machine, lean,
// remote, assist, smoothing, stopping, faults) against simulated seat sensors
// and a simple model of the two wheels. No hardware, no motors.
#include "lean_test_support.h"
#include "DriveController.h"
#include "drive_knobs.h"
#include <cstring>
#include <vector>

using namespace common;
using Kind = DriveController::MotorCommand::Kind;
using Fault = DriveController::Fault;

static bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

struct Sim
{
    DriveController drive;
    uint32_t now = 0;
    float left = 0, right = 0; // the wheels, RPM
    bool leftAlive = true, rightAlive = true;
    float knobMS = knobs::speedLimitMS(1); // speed-knob position 2, where the team drives
    // The simulated VESC speed loop: wheel acceleration is followGain x the
    // speed error, up to followRPMS (the motor's torque limit).
    float followRPMS = 300.0f, followGain = 60.0f;
    // Below this the simulated VESC lets go of the motor (s_pid_min_erpm:
    // VESC Tool default 900 ERPM = 15.5 wheel RPM).
    float vescMinRPM = 15.5f;
    // The VESC speed PID's "Allow Braking". Off: a speed command below the
    // wheel's speed only lets the wheel coast.
    bool vescAllowBraking = true;
    bool calHeld = false;
    ModeStateMachine::Inputs pending;
    DriveController::MotorCommand last{{Kind::Current, 0}, {Kind::Current, 0}};
    // Added to each wheel every tick, in RPM per second: a slope, or a push on the rims.
    float externalRPMS = 0.0f;
    std::vector<DriveController::MotorCommand> trace;

    explicit Sim(bool afterWatchdogReset = false)
    {
        drive.begin(afterWatchdogReset);
        drive.applySettings();
    }

    // A VESC in speed mode follows the command, but lets go of the motor below
    // its minimum ERPM (15.5 wheel RPM). Brake current slows the wheel. Zero
    // current coasts against a little friction; assist current accelerates.
    void wheel(float &rpm, Kind kind, float value) const
    {
        const float dt = TickS;
        const auto slow = [&](float rate) { rpm -= std::copysign(std::fmin(rate * dt, std::fabs(rpm)), rpm); };
        const bool wouldBrake = value * rpm > 0 && std::fabs(value) < std::fabs(rpm);
        if (kind == Kind::RPM && std::fabs(value) >= vescMinRPM && (vescAllowBraking || !wouldBrake))
            rpm += std::fmin(followRPMS, std::fmax(-followRPMS, followGain * (value - rpm))) * dt;
        else if (kind == Kind::Brake)
            slow(value * 100.0f / STOP_BRAKE_AMPS); // 15 A slows the wheel at 100 RPM/s
        else
        {
            if (kind == Kind::Current)
                rpm += value * 1.5f * dt;
            slow(4.0f);
        }
    }

    void tick()
    {
        now += TickMs;
        ModeStateMachine::Inputs input = pending;
        pending = {};
        drive.updateButtons(input, now);
        DriveController::TickInput in;
        in.now = now;
        in.dt = TickS;
        in.leftRPM = left;
        in.rightRPM = right;
        in.leftFresh = leftAlive;
        in.rightFresh = rightAlive;
        in.speedLimitMS = knobMS;
        in.calButtonHeld = calHeld;
        last = drive.tick(in);
        trace.push_back(last);
        wheel(left, last.left.kind, last.left.value);
        wheel(right, last.right.kind, last.right.value);
        left += externalRPMS * TickS;
        right += externalRPMS * TickS;
    }
    void run(uint32_t ms)
    {
        for (uint32_t t = 0; t < ms; t += TickMs)
            tick();
    }
    // A tap: the ISR edge arrives first, the debounced press a loop later, then the release.
    void press(mode button, bool release = true)
    {
        const auto pick = [&]() -> ModeStateMachine::Button & {
            return button == LEAN ? pending.lean : button == REMOTE ? pending.remote : pending.disarm;
        };
        pick().edge = true;
        tick();
        pick().pressed = true;
        tick();
        if (release)
        {
            pick().released = true;
            tick();
        }
    }
    // Press and keep holding (DISARM held for half a second selects Assist).
    void hold(mode button) { press(button, /*release=*/false); }
    void release(mode button)
    {
        (button == LEAN ? pending.lean : button == REMOTE ? pending.remote : pending.disarm).released = true;
        tick();
    }
    void pressCal(uint32_t holdMs = 50)
    {
        pending.calibrate.pressed = true;
        calHeld = true;
        run(holdMs);
        calHeld = false;
        tick();
    }
    void remote(float l, float r)
    {
        uint8_t packet[5] = {0x01};
        const int16_t li = static_cast<int16_t>(l * 32767), ri = static_cast<int16_t>(r * 32767);
        memcpy(packet + 1, &li, 2);
        memcpy(packet + 3, &ri, 2);
        drive.onRemotePacket(packet, sizeof(packet), now);
    }
    // Select REMOTE with a rider seated and the stick centred, ready to drive.
    void rideRemote()
    {
        lean(0, 0);
        run(400);
        press(REMOTE);
        assert(drive.mode() == REMOTE);
        runRemote(50, 0, 0);
        assert(!drive.remoteStopped());
    }
    // Remote driving: a packet every 10 ms, as the controller sends them.
    void runRemote(uint32_t ms, float l, float r)
    {
        for (uint32_t t = 0; t < ms; t += TickMs)
        {
            if (t % 10 == 0)
                remote(l, r);
            tick();
        }
    }
    // Sit down, select LEAN and capture neutral.
    void rideLean()
    {
        lean(0, 0);
        run(400);
        assert(drive.lean().isSeated());
        press(LEAN);
        assert(drive.mode() == LEAN && last.left.kind == Kind::Current && last.left.value == 0);
        pressCal();
        run(1500);
        assert(drive.lean().calibrated());
    }
    // Checks a stop from here: wheel-speed commands only ever fall, then brake
    // current, then the stop is over. Returns the seconds it took.
    float expectStop(bool expectBrake = true)
    {
        float previous = std::fmax(std::fabs(left), std::fabs(right)) + MAX_RPM_COMMAND_DIFF;
        bool braked = false;
        const uint32_t start = now;
        for (int k = 0; k < 2000; ++k)
        {
            tick();
            if (!drive.stopping())
            {
                assert(braked == expectBrake);
                // Afterwards the chair is either released or held, never driven.
                assert(last.left.kind != Kind::RPM && last.right.kind != Kind::RPM);
                if (last.left.kind == Kind::Current)
                    assert(last.left.value == 0 && last.right.value == 0);
                return (now - start) / 1000.0f;
            }
            if (last.left.kind == Kind::RPM)
            {
                assert(!braked && last.right.kind == Kind::RPM);
                const float peak = std::fmax(std::fabs(last.left.value), std::fabs(last.right.value));
                assert(peak <= previous + 0.001f);
                previous = peak;
            }
            else
            {
                braked = true;
                assert(last.left.kind == Kind::Brake && last.right.kind == Kind::Brake);
                assert(last.left.value == STOP_BRAKE_AMPS && last.right.value == STOP_BRAKE_AMPS);
            }
        }
        assert(!"stop never finished");
        return 0;
    }
};

static float yawDegS(const Sim &sim)
{ // from the wheel-speed commands and the configured track width
    const float differenceMS = (sim.last.left.value - sim.last.right.value) / LINEAR_TO_RPM;
    return differenceMS / settings::trackWidth.get() * 180.0f / 3.14159265f;
}
static float speedMS(const Sim &sim) { return (sim.last.left.value + sim.last.right.value) / 2.0f / LINEAR_TO_RPM; }

int main()
{
    settings::persistence.load();
    const float turnRate = settings::maxTurnRate.get();

    { // LEAN from power-on: nothing moves until seated, selected and calibrated.
        Sim sim;
        assert(sim.drive.mode() == DISARM);
        loads(0, 65, 0, 0); // empty seat
        sim.run(500);
        sim.press(LEAN); // ignored: nobody is seated
        assert(sim.drive.mode() == DISARM);
        lean(400, 0); // sits down already leaning
        sim.run(400);
        sim.press(LEAN);
        assert(sim.drive.mode() == LEAN);
        sim.run(1000);
        assert(sim.last.left.kind == Kind::Current && sim.last.left.value == 0 && sim.left == 0);
        lean(0, 0);
        sim.pressCal();
        sim.run(1500);
        assert(sim.drive.lean().calibrated());
        // Neutral: a zero wheel-speed command, and the chair has not moved.
        assert(sim.last.left.kind == Kind::RPM && sim.last.left.value == 0 && sim.last.right.value == 0 && sim.left == 0);
        lean(600, 0); // full forward
        sim.run(2500);
        const float top = sim.knobMS * LINEAR_TO_RPM;
        assert(sim.last.left.kind == Kind::RPM);
        assert(near(sim.last.left.value, top, 0.5f) && near(sim.last.right.value, top, 0.5f));
        assert(near(sim.left, top, 1.0f));
        assert(sim.drive.fault() == Fault::None);
    }
    { // Steering works while the chair is still accelerating, and stops when released.
        Sim sim;
        sim.rideLean();
        lean(600, 250);
        sim.run(250);
        // Forward speed is still building toward 78 RPM, but the turn is already there.
        assert(sim.last.left.kind == Kind::RPM);
        assert((sim.last.left.value + sim.last.right.value) / 2 < 60.0f);
        const float turning = yawDegS(sim);
        assert(turning > 20.0f);
        lean(600, 0); // straighten up, keep driving
        sim.run(250);
        assert(std::fabs(yawDegS(sim)) < 1.0f);
        assert((sim.last.left.value + sim.last.right.value) / 2 > 40.0f);
    }
    { // Full sideways lean turns at the configured rate on every speed-knob position.
        for (int position = 0; position < knobs::Positions; ++position)
        {
            Sim sim;
            sim.knobMS = knobs::speedLimitMS(position);
            sim.rideLean();
            lean(0, 500);
            sim.run(1500);
            assert(near(yawDegS(sim), turnRate, 1.0f));
            assert(near(speedMS(sim), 0.0f, 0.01f)); // on the spot
            // The rate is reached within CDR Spec H's half second...
            Sim quick;
            quick.knobMS = knobs::speedLimitMS(position);
            quick.rideLean();
            lean(0, -500);
            quick.run(500);
            assert(near(yawDegS(quick), -turnRate, 2.0f));
            // ...and after straightening up the rotation is gone in a fifth of a second.
            lean(0, 0);
            quick.run(TickMs * 2 + static_cast<uint32_t>(1000.0f * turnRate / TURN_DECEL_DEG_S2) + 30);
            assert(std::fabs(yawDegS(quick)) < 0.5f || quick.last.left.kind != Kind::RPM);
        }
    }
    { // At speed the turn rate is capped by sideways acceleration.
        Sim sim;
        sim.knobMS = knobs::speedLimitMS(3);
        sim.rideLean();
        lean(600, 0);
        sim.run(3000);
        lean(600, 500); // full speed, full turn
        float worst = 0;
        for (int k = 0; k < 600; ++k)
        {
            sim.tick();
            const float lateral = std::fabs(speedMS(sim) * yawDegS(sim) * 3.14159265f / 180.0f);
            worst = std::fmax(worst, lateral);
        }
        assert(worst <= settings::maxLateralAccel.get() * 1.05f);
        assert(yawDegS(sim) > 20.0f && yawDegS(sim) < turnRate / 2); // turning, but far below the standstill rate
        assert(std::fabs(sim.last.left.value) <= sim.knobMS * LINEAR_TO_RPM + 0.01f);
    }
    { // A VESC goes silent at speed: fault, DISARM, ramp down, brake, release.
        for (int which = 0; which < 2; ++which)
        {
            Sim sim;
            sim.rideLean();
            lean(600, 0);
            sim.run(2500);
            messages.clear();
            (which ? sim.rightAlive : sim.leftAlive) = false;
            sim.tick();
            assert(sim.drive.mode() == DISARM && sim.drive.fault() == Fault::VescSilent);
            assert(logged("motor controller stopped reporting") && logged("Mode: DISARM"));
            assert(sim.last.left.kind == Kind::RPM && sim.last.left.value < 78.0f); // already slowing on the first tick
            assert(sim.drive.stopping());
            const float seconds = sim.expectStop();
            // Ramp from 78 RPM at STOP_DECEL (0.53 s), then the full blind brake phase.
            assert(seconds > 0.5f && seconds < 0.6f + STOP_BRAKE_MAX_MS / 1000.0f);
            // Stays disarmed; a new selection is refused while the VESC is silent.
            messages.clear();
            sim.press(LEAN);
            sim.run(100);
            assert(sim.drive.mode() == DISARM && sim.drive.fault() == Fault::VescSilent);
            // Never driven. After the fault the chair is held (brake current on both
            // wheels, since there is no wheel speed to go by) until DISARM is pressed.
            assert(sim.last.left.kind == Kind::Brake && sim.last.right.kind == Kind::Brake);
            sim.press(REMOTE);
            sim.runRemote(200, 1, 1);
            assert(sim.drive.mode() == DISARM && sim.last.left.kind == Kind::Brake);
            sim.press(DISARM); // acknowledged, rider aboard: a manual chair again
            assert(sim.last.left.kind == Kind::Current && sim.last.left.value == 0);
            // Feedback back: arming works again and the fault clears.
            sim.leftAlive = sim.rightAlive = true;
            sim.press(LEAN);
            assert(sim.drive.mode() == LEAN && sim.drive.fault() == Fault::None);
        }
    }
    { // The rider leaves the seat at speed: the chair brakes to a stop by itself.
        Sim sim;
        sim.rideLean();
        lean(600, 0);
        sim.run(2500);
        loads(0, 65, 0, 0);
        sim.run(90);
        assert(sim.drive.mode() == LEAN); // a bump this short is ignored
        sim.run(30);
        assert(sim.drive.mode() == DISARM); // empty for 0.1 s
        const float seconds = sim.expectStop();
        assert(seconds < 1.0f && std::fabs(sim.left) < STOP_DONE_RPM);
        // Sitting back down returns to LEAN, but it needs a fresh calibration.
        lean(600, 0);
        sim.run(500);
        assert(sim.drive.mode() == LEAN && !sim.drive.lean().calibrated());
        sim.run(1000);
        assert(sim.last.left.kind == Kind::Current && sim.left == 0);
    }
    { // A seat sensor fails at speed: fault, DISARM, braking stop.
        Sim sim;
        sim.rideLean();
        lean(600, 0);
        sim.run(2500);
        messages.clear();
        sensorInputs[PIN_F] = 4095;
        sim.run(15);
        assert(sim.drive.mode() == LEAN); // three samples
        sim.tick();
        assert(sim.drive.mode() == DISARM && sim.drive.fault() == Fault::SensorRail);
        assert(logged("seat sensor is reading at an ADC rail"));
        // The stuck-high reading keeps the seat "occupied"; the chair stays disarmed anyway.
        assert(sim.expectStop() < 1.0f);
        sim.run(500);
        assert(sim.drive.mode() == DISARM && sim.last.left.kind == Kind::Current && sim.left == 0);
        sim.press(LEAN);
        sim.run(100);
        assert(sim.drive.mode() == DISARM);
    }
    { // DISARM at speed brakes; DISARM at a standstill just releases.
        Sim sim;
        sim.rideLean();
        lean(600, 250);
        sim.run(2500);
        sim.press(DISARM);
        assert(sim.drive.mode() == DISARM && sim.drive.stopping());
        assert(sim.expectStop() < 1.0f);
        assert(sim.drive.fault() == Fault::None);

        // Holding the DISARM button down (people do, with a stop button) stays a
        // stop: it never turns into Assist, and a nudge afterwards gets no boost.
        Sim held;
        held.knobMS = knobs::speedLimitMS(3);
        held.rideLean();
        lean(600, 0);
        held.run(3500);
        held.hold(DISARM);
        held.run(600);
        assert(held.drive.mode() == DISARM && held.drive.stopping());
        held.expectStop();
        held.run(2500);
        assert(held.drive.mode() == DISARM);
        held.left = held.right = 5.0f; // rolled by a slope or a bump
        held.run(200);
        assert(held.drive.mode() == DISARM && held.last.left.kind == Kind::Current && held.last.left.value == 0);
        // Assist needs its own press, starting from DISARM, a second or more after the stop.
        held.release(DISARM);
        held.left = held.right = 0;
        held.hold(DISARM);
        held.run(520);
        assert(held.drive.mode() == ASSIST);
        held.release(DISARM);
        held.run(500);
        assert(held.last.left.kind == Kind::Current && held.last.left.value == 0); // at rest: no boost

        Sim still;
        still.rideLean();
        still.press(DISARM);
        assert(still.drive.mode() == DISARM && !still.drive.stopping());
        assert(still.last.left.kind == Kind::Current && still.last.left.value == 0);
    }
    { // The calibration button bumped at speed is ignored. A chair that is only creeping
      // is stopped first and then calibrated. DISARM works during calibration.
        Sim sim;
        sim.rideLean();
        lean(600, 0);
        sim.run(2500);
        messages.clear();
        const float cruising = sim.left;
        sim.pressCal();
        sim.run(500);
        assert(!sim.drive.lean().calibrationActive() && sim.drive.lean().calibrated() && !sim.drive.stopping());
        assert(near(sim.left, cruising, 0.5f) && logged("Calibration ignored: the chair is moving"));
        // Creeping at about 10 RPM because neutral is off: this is when the button is needed.
        sim.vescMinRPM = 3.0f;
        lean(200, 0);
        sim.run(2500);
        assert(sim.left > 5.0f && sim.left < CALIBRATE_MAX_RPM);
        sim.pending.calibrate.pressed = true;
        sim.calHeld = true;
        sim.tick();
        assert(sim.drive.lean().calibrationActive());
        sim.run(40);
        sim.calHeld = false;
        sim.run(2500); // stopped, then a still second in this posture
        assert(sim.drive.lean().calibrated() && sim.drive.mode() == LEAN);
        assert(std::fabs(sim.left) < 1.0f); // this posture is neutral now
        sim.vescMinRPM = 15.5f;
        // DISARM during a calibration ends it and disarms.
        sim.pressCal(1200); // long press: range calibration
        sim.run(1500);
        assert(sim.drive.lean().calibrationActive());
        sim.press(DISARM);
        assert(sim.drive.mode() == DISARM && !sim.drive.lean().calibrationActive());
    }
    { // Range calibration through the controller: saved, and it ends in DISARM.
        Sim sim;
        lean(0, 0);
        sim.run(400);
        sim.press(LEAN);
        sim.pressCal(1200);
        sim.run(1500);
        lean(500, 0);
        sim.run(600);
        lean(-300, 0);
        sim.run(600);
        lean(0, -400);
        sim.run(600);
        lean(0, 350);
        sim.run(600);
        lean(0, 0);
        sim.run(300);
        assert(sim.last.left.kind == Kind::Current && sim.left == 0); // motors off throughout
        sim.pending.calibrate.pressed = true;
        sim.tick();
        assert(sim.drive.mode() == DISARM);
        assert(settings::rangeForward.get() == 450 && settings::rangeBack.get() == 270);
        assert(settings::rangeLeft.get() == 360 && settings::rangeRight.get() == 315);
        // Survives a power cycle: the bank on EEPROM has them.
        assert(!settings::persistence.dirty());
        settings::rangeForward.reset();
        settings::persistence.load();
        assert(settings::rangeForward.get() == 450 && settings::rangeRight.get() == 315);
        // Back in LEAN a fresh neutral is needed before driving.
        sim.press(LEAN);
        assert(sim.drive.mode() == LEAN && !sim.drive.lean().calibrated());
        settings::registry().defaults();
        settings::persistence.refreshDirty();
        assert(settings::persistence.save());
    }
    { // Tare from the panel: DISARM, empty seat, hold calibrate for two seconds.
        Sim sim;
        settings::persistence.refreshDirty();
        settings::persistence.save();
        rawSensors(372, 437, 380, 365);
        sim.run(300);
        sim.pressCal(500); // too short: nothing
        sim.run(1500);
        assert(settings::zeroBack.get() == 372);
        sim.pressCal(3200);
        assert(settings::zeroBack.get() == 437 && settings::zeroLeft.get() == 380);
        settings::zeroBack.reset();
        settings::persistence.load();
        assert(settings::zeroBack.get() == 437); // saved
        assert(sim.drive.mode() == DISARM && sim.last.left.kind == Kind::Current && sim.last.left.value == 0);

        // With someone on the seat a tare is refused: it would hide that rider.
        messages.clear();
        rawSensors(505, 430, 401, 481); // a seated rider's logged readings
        sim.run(400);
        assert(sim.drive.lean().isSeated());
        sim.pressCal(3500);
        assert(logged("Tare refused: the seat reads as occupied") && !sim.drive.lean().tareActive());
        assert(settings::zeroFront.get() == 372 && settings::zeroBack.get() == 437);
        // Sitting down while a tare is under way cancels it.
        rawSensors(372, 437, 380, 365);
        sim.run(300);
        sim.pending.calibrate.pressed = true;
        sim.calHeld = true;
        sim.run(1000);
        assert(sim.drive.lean().tareActive());
        rawSensors(505, 430, 401, 481);
        sim.run(500);
        assert(!sim.drive.lean().tareActive() && logged("Tare cancelled"));
        sim.run(3000);
        sim.calHeld = false;
        assert(settings::zeroFront.get() == 372 && settings::zeroLeft.get() == 380);
        // A tare never starts in LEAN (the same button calibrates the lean there).
        settings::registry().defaults();
        settings::persistence.refreshDirty();
        settings::persistence.save();
    }
    { // REMOTE: drives on packets, brakes when they stop, picks up smoothly if they return.
        Sim sim;
        loads(0, 65, 0, 0);
        sim.run(400);
        sim.press(REMOTE); // mode buttons other than DISARM need someone in the seat
        assert(sim.drive.mode() == DISARM);
        lean(0, 0);
        sim.run(400);
        sim.press(REMOTE);
        assert(sim.drive.mode() == REMOTE);
        loads(0, 65, 0, 0); // REMOTE keeps running with the seat empty
        sim.run(400);
        assert(sim.drive.mode() == REMOTE);
        sim.run(300);
        assert(sim.last.left.kind == Kind::Current && sim.drive.remoteStopped());
        // A stick that is already held over when REMOTE is selected does nothing...
        sim.runRemote(1000, 1.0f, 1.0f);
        assert(sim.drive.remoteStopped() && sim.last.left.kind == Kind::Current && sim.left == 0);
        // ...until it has been centred once.
        sim.runRemote(30, 0.0f, 0.0f);
        assert(!sim.drive.remoteStopped());
        sim.runRemote(2500, 1.0f, 1.0f);
        const float top = sim.knobMS * LINEAR_TO_RPM;
        assert(near(sim.last.left.value, top, 0.5f) && !sim.drive.remoteStopped());
        // Controller lost: after the 100 ms timeout the chair brakes to a stop.
        sim.run(80); // the last packet was 10 ms before this
        assert(sim.last.left.kind == Kind::RPM && near(sim.last.left.value, top, 0.5f));
        sim.run(15);
        assert(sim.drive.remoteStopped() && sim.drive.stopping() && sim.drive.mode() == REMOTE);
        const float seconds = sim.expectStop();
        assert(seconds < 1.0f && std::fabs(sim.left) < STOP_DONE_RPM);

        // A short dropout: packets return mid-ramp and the command carries on from there.
        sim.runRemote(30, 0.0f, 0.0f); // (that stop outlasted half a second: centre the stick first)
        sim.runRemote(2500, 1.0f, 1.0f);
        sim.run(250);
        assert(sim.drive.stopping() && sim.last.left.kind == Kind::RPM);
        const float during = sim.last.left.value;
        assert(during < top - 10.0f && during > STOP_HANDOFF_RPM);
        sim.remote(1.0f, 1.0f);
        sim.tick();
        assert(!sim.drive.stopping() && sim.last.left.kind == Kind::RPM);
        assert(near(sim.last.left.value, during, 2.0f)); // no jump back to zero or up to full
        sim.runRemote(2000, 1.0f, 1.0f);
        assert(near(sim.last.left.value, top, 0.5f));

        // A real outage (3 s) with the stick held forward: the chair has stopped,
        // and the link coming back does not start it again.
        sim.run(3000);
        assert(std::fabs(sim.left) < STOP_DONE_RPM);
        sim.runRemote(1500, 1.0f, 1.0f);
        assert(sim.drive.remoteStopped() && sim.last.left.kind == Kind::Current && sim.left == 0);
        sim.runRemote(30, 0.0f, 0.0f); // stick centred
        sim.runRemote(2000, 1.0f, 1.0f);
        assert(near(sim.last.left.value, top, 0.5f));

        // Spin on the spot and tank steering work through the two-axis smoother, inside
        // the same turn-rate and sideways-acceleration limits as LEAN.
        sim.runRemote(2500, 0.2f, -0.2f); // a gentle spin: as asked
        assert(near(sim.last.left.value, 0.2f * top, 0.5f) && near(sim.last.right.value, -0.2f * top, 0.5f));
        sim.runRemote(2500, 0.5f, -0.5f); // asks for 235 deg/s: held to the full-lean turn rate
        assert(near(yawDegS(sim), settings::maxTurnRate.get(), 1.0f) && near(speedMS(sim), 0.0f, 0.01f));
        sim.runRemote(2500, 1.0f, 0.4f);
        assert(sim.last.left.value > sim.last.right.value + 5.0f);
        assert(std::fabs(speedMS(sim) * yawDegS(sim) * 3.14159265f / 180.0f) <= settings::maxLateralAccel.get() * 1.05f);
        // Full back stick: reverse at the reverse share of the limit.
        sim.runRemote(4000, -1.0f, -1.0f);
        assert(near(sim.last.left.value, -REVERSE_SPEED_FRACTION * top, 0.5f) && near(sim.last.right.value, sim.last.left.value, 0.01f));
        sim.runRemote(3000, 0.0f, 0.0f);
        sim.runRemote(2500, 1.0f, 1.0f);

        // Soft stop from the controller brakes too.
        const uint8_t softStop = 0x02;
        sim.drive.onRemotePacket(&softStop, 1, sim.now);
        sim.tick();
        assert(sim.drive.stopping());
        sim.expectStop();
        // Clearing the soft stop with the stick still over does not start the chair either.
        const uint8_t clear = 0x03;
        sim.drive.onRemotePacket(&clear, 1, sim.now);
        sim.runRemote(1000, 1.0f, 0.4f);
        assert(sim.drive.remoteStopped() && sim.left == 0);
        sim.runRemote(30, 0.0f, 0.0f);
        sim.runRemote(1000, 1.0f, 1.0f);
        assert(!sim.drive.remoteStopped() && sim.left > 20.0f);
    }
    { // REMOTE, stick hard over from full speed on every knob position: the pivot stays
      // inside the sideways-acceleration limit (it reached 5, 11 and 20 m/s^2 on knobs 2 to 4).
        for (int position = 0; position < knobs::Positions; ++position)
        {
            Sim sim;
            sim.knobMS = knobs::speedLimitMS(position);
            sim.rideRemote();
            sim.runRemote(4000, 1.0f, 1.0f);
            float worst = 0, fastestYaw = 0;
            for (int k = 0; k < 600; ++k)
            {
                if (k % 2 == 0)
                    sim.remote(1.0f, 0.0f);
                sim.tick();
                const float wheelSpeed = (sim.left + sim.right) / 2 / LINEAR_TO_RPM;
                const float wheelYaw = (sim.left - sim.right) / LINEAR_TO_RPM / settings::trackWidth.get();
                worst = std::fmax(worst, std::fabs(wheelSpeed * wheelYaw));
                fastestYaw = std::fmax(fastestYaw, std::fabs(wheelYaw) * 180.0f / 3.14159265f);
            }
            assert(worst <= settings::maxLateralAccel.get() * 1.15f); // measured on the wheels, which lag a little
            assert(fastestYaw <= settings::maxTurnRate.get() + 5.0f && fastestYaw > 20.0f);
        }
    }
    { // Engaging on a rolling chair starts from the wheels' speed instead of braking to zero.
        Sim sim;
        sim.rideRemote();
        sim.left = sim.right = 60.0f; // rolling
        sim.tick(); // stick centred, chair moving: stop under way
        sim.remote(1.0f, 1.0f);
        sim.tick();
        assert(sim.last.left.kind == Kind::RPM && near(sim.last.left.value, 60.0f, 3.0f));
        sim.runRemote(100, 1.0f, 1.0f);
        assert(sim.last.left.value > 60.0f);
    }
    { // The speed command never strays more than 50 RPM from the wheel.
        Sim sim;
        sim.rideRemote();
        for (int k = 0; k < 600; ++k)
        {
            if (k % 2 == 0)
                sim.remote(1.0f, 1.0f);
            const float before = sim.left;
            sim.left = 0; // wheels held (stalled against something)
            sim.right = 0;
            (void)before;
            sim.tick();
            assert(sim.last.left.kind == Kind::RPM && std::fabs(sim.last.left.value) <= MAX_RPM_COMMAND_DIFF + 0.001f);
        }
    }
    { // ASSIST: a boost has to be earned by a push, and the motor can never keep the chair going.
        const auto enterAssist = [](Sim &sim, float rollingRPM = 0.0f) {
            lean(0, 0);
            sim.run(400);
            sim.left = sim.right = rollingRPM; // already rolling when Assist is selected
            sim.hold(DISARM);
            sim.run(600); // held: Assist after half a second
            assert(sim.drive.mode() == ASSIST);
            sim.release(DISARM);
        };
        // A push on the rims: the wheels speed up at this rate for this long.
        const auto push = [](Sim &sim, float accelMS2, uint32_t ms) {
            sim.externalRPMS = accelMS2 * LINEAR_TO_RPM;
            sim.run(ms);
            sim.externalRPMS = 0;
        };
        const auto boostSeconds = [](Sim &sim, uint32_t ms) {
            float seconds = 0;
            for (uint32_t t = 0; t < ms; t += TickMs)
            {
                sim.tick();
                assert(sim.last.left.kind == Kind::Current && sim.last.right.kind == Kind::Current);
                assert(sim.last.left.value >= 0); // never against the direction of travel
                if (sim.last.left.value > 0)
                    seconds += TickS;
            }
            return seconds;
        };
        { // No push, no boost: the chair cannot start or sustain itself, at any speed.
            for (float start : {0.0f, 5.0f, 20.0f, 39.0f, 60.0f})
            {
                Sim sim;
                enterAssist(sim, start);
                const float entered = sim.left;
                float fastest = entered;
                for (int k = 0; k < 4000; ++k)
                {
                    sim.tick();
                    assert(sim.last.left.kind == Kind::Current && sim.last.left.value == 0 && sim.last.right.value == 0);
                    fastest = std::fmax(fastest, sim.left);
                }
                assert(fastest == entered && sim.left <= entered); // only ever slows down
            }
            // Rolling down a slope is not a push, however steep: the speeding-up never ends.
            // (1 in 12 is 0.8 m/s^2; a 15% curb cut is 1.5; and steeper still.)
            for (float slopeMS2 : {0.75f, 1.5f, 3.0f})
            {
                Sim ramp;
                enterAssist(ramp);
                ramp.externalRPMS = slopeMS2 * LINEAR_TO_RPM; // let go at the top
                assert(boostSeconds(ramp, 5000) == 0);
            }
        }
        { // A real push gets one boost, which fades out in 0.7 s and is not renewed by itself.
            Sim sim;
            enterAssist(sim);
            sim.externalRPMS = 2.0f * LINEAR_TO_RPM; // 2 m/s^2 for 0.3 s: about 19 RPM
            assert(boostSeconds(sim, 300) == 0);     // nothing while the rider is still pushing
            sim.externalRPMS = 0;
            const float afterPush = sim.left;
            assert(afterPush > 15.0f);
            float boosted = 0, lastBoostAt = 0, peakAmps = 0;
            for (int k = 0; k < 1000; ++k) // five seconds, hands off
            {
                sim.tick();
                if (sim.last.left.value > 0)
                {
                    boosted += TickS;
                    lastBoostAt = k * TickS;
                    peakAmps = std::fmax(peakAmps, sim.last.left.value);
                }
            }
            // The boost begins when the push ends: on every other tick for 0.7 s.
            assert(boosted > 0.3f && boosted < 0.36f);
            assert(lastBoostAt < 1.0f && peakAmps > 2.0f && peakAmps <= settings::assistMaxBoostAmps.get());
            assert(sim.left < afterPush);                    // and the chair is slowing down again
            // Pushing again and again gets a boost each time.
            Sim rhythm;
            rhythm.knobMS = knobs::speedLimitMS(3);
            enterAssist(rhythm);
            for (int stroke = 0; stroke < 5; ++stroke)
            {
                bool boosted = false;
                rhythm.externalRPMS = 2.0f * LINEAR_TO_RPM;
                for (int k = 0; k < 260; ++k) // 0.3 s push, 1 s recovery
                {
                    if (k == 60)
                        rhythm.externalRPMS = 0;
                    rhythm.tick();
                    boosted = boosted || rhythm.last.left.value > 0;
                }
                assert(boosted);
                assert(rhythm.last.left.value == 0); // and it is over before the next stroke
            }
        }
        { // The boost is on half the time whatever the control period is set to.
            for (uint32_t period : {2u, 4u, 5u, 10u, 15u})
            {
                PushAssist assist;
                uint32_t now = 0;
                float rpm = 0;
                int on = 0, ticks = 0;
                for (int k = 0; k < static_cast<int>(3000 / period); ++k)
                {
                    now += period;
                    if (now < 400)
                        rpm += 2.0f * LINEAR_TO_RPM * period / 1000.0f; // the push
                    const float amps = assist.update(rpm, now, 155.0f);
                    if (assist.boostActive())
                    {
                        ++ticks;
                        on += amps > 0;
                    }
                }
                assert(ticks > 20 && std::abs(2 * on - ticks) <= 2);
            }
        }
        { // Slowing the chair by hand is not fought for long, and never restarts a boost.
            Sim sim;
            enterAssist(sim);
            push(sim, 2.0f, 300);
            sim.externalRPMS = -0.5f * LINEAR_TO_RPM; // hands braking on the rims
            const float seconds = boostSeconds(sim, 3000);
            assert(seconds < 0.36f);
            sim.externalRPMS = 0;
            assert(sim.left < 1.0f);
        }
        { // At and above the speed-knob limit a push earns nothing.
            Sim sim;
            enterAssist(sim);
            sim.left = sim.right = sim.knobMS * LINEAR_TO_RPM;
            sim.run(200);
            push(sim, 2.0f, 300);
            assert(boostSeconds(sim, 1000) == 0);
        }
        { // DISARM in Assist stops the chair, like everywhere else.
            Sim sim;
            enterAssist(sim);
            sim.left = sim.right = 40.0f;
            sim.run(50);
            sim.press(DISARM);
            assert(sim.drive.mode() == DISARM && sim.drive.stopping());
            assert(sim.expectStop() < 1.0f && std::fabs(sim.left) < STOP_DONE_RPM);
            // A tap never reaches Assist, and after a stop a quick second press held
            // down does not either: the hold only counts a second after the stop.
            sim.run(700);
            assert(sim.drive.mode() == DISARM);
            Sim panic;
            panic.rideLean();
            lean(600, 0);
            panic.run(2500);
            panic.press(DISARM);
            panic.hold(DISARM); // pressed again at once and held
            panic.run(3000);
            assert(panic.drive.mode() == DISARM && panic.left == 0);
        }
        { // Seat empty in Assist with the chair rolling: brake, and Assist does not
          // come back by itself when someone sits down (the LED says DISARM).
            Sim sim;
            enterAssist(sim);
            sim.left = sim.right = 40.0f;
            loads(0, 65, 0, 0);
            sim.run(120);
            assert(sim.drive.mode() == DISARM && sim.drive.stopping());
            sim.expectStop();
            lean(0, 0);
            sim.run(1000);
            assert(sim.drive.mode() == DISARM);
            push(sim, 2.0f, 300); // someone climbing in shoves the chair
            sim.run(500);
            assert(sim.drive.mode() == DISARM && sim.last.left.kind == Kind::Current && sim.last.left.value == 0);
        }
        { // A silent VESC in Assist: disarmed. No boost on stale speed.
            Sim sim;
            enterAssist(sim);
            sim.leftAlive = false;
            sim.tick();
            assert(sim.drive.mode() == DISARM && sim.drive.fault() == Fault::VescSilent);
            assert(sim.last.left.kind != Kind::Current || sim.last.left.value == 0);
        }
    }
    { // Turning the speed knob down at speed slows the chair at the design rate, not in a step.
        Sim sim;
        sim.knobMS = knobs::speedLimitMS(3);
        sim.rideLean();
        lean(600, 0);
        sim.run(3500);
        const float top = knobs::speedLimitMS(3) * LINEAR_TO_RPM;
        assert(near(sim.last.left.value, top, 0.5f));
        sim.knobMS = knobs::speedLimitMS(0);
        float previous = sim.last.left.value;
        for (int k = 0; k < 400; ++k)
        {
            sim.tick();
            assert(previous - sim.last.left.value <= MAX_ACCEL * LINEAR_TO_RPM * TickS + 0.01f);
            previous = sim.last.left.value;
        }
        assert(near(sim.last.left.value, knobs::speedLimitMS(0) * LINEAR_TO_RPM, 0.5f));
    }
    { // A heavy chair that accelerates slower than the command asks. The command
      // runs ahead of the wheels; none of what follows may make that worse.
        const auto sluggish = [](Sim &sim) {
            sim.knobMS = knobs::speedLimitMS(3);
            sim.followRPMS = 40.0f; // about 1.3 m/s^2, a third of what is commanded
            sim.rideLean();
        };
        { // Releasing the lean slows the chair at once: it does not carry on speeding up.
            Sim sim;
            sluggish(sim);
            lean(600, 0);
            sim.run(1500);
            assert(sim.last.left.value > sim.left + 40.0f); // the command is well ahead of the wheel
            const float atRelease = sim.left;
            lean(0, 0);
            float peak = atRelease;
            for (int k = 0; k < 400; ++k)
            {
                sim.tick();
                peak = std::fmax(peak, sim.left);
            }
            // Only the lean filter's 25 ms and one tick of reaction: under 2 RPM more.
            assert(peak < atRelease + 2.0f);
            assert(sim.left < atRelease / 2);
        }
        { // The command never drops into the VESC's dead zone while the wheel is still fast
          // (the simulated VESC, like the real one, lets go of the motor below 15.5 RPM).
            Sim sim;
            sluggish(sim);
            lean(600, 0);
            sim.run(4000);
            assert(sim.left > 100.0f);
            lean(0, 0);
            for (int k = 0; k < 1200 && sim.left > STOP_HANDOFF_RPM + 4.0f; ++k)
            {
                const float before = sim.left;
                sim.tick();
                assert(sim.last.left.kind == Kind::RPM && sim.last.left.value >= STOP_HANDOFF_RPM);
                assert(sim.left < before); // braked all the way down, never coasting
            }
            assert(sim.left <= STOP_HANDOFF_RPM + 4.0f);
            sim.run(500);
            assert(std::fabs(sim.last.left.value) < 1.0f); // then the command goes to zero as before
        }
        { // The same for a turn: straightening up stops the rotation building.
            Sim sim;
            sluggish(sim);
            lean(0, 500);
            sim.run(400);
            const float atRelease = sim.left - sim.right;
            assert(atRelease > 10.0f && (sim.last.left.value - sim.last.right.value) > atRelease + 4.0f);
            lean(0, 0);
            float peak = atRelease;
            for (int k = 0; k < 200; ++k)
            {
                sim.tick();
                peak = std::fmax(peak, sim.left - sim.right);
            }
            assert(peak < atRelease + 1.5f);
        }
        { // Steering still gets through while the forward command is far ahead.
          // Here the wheels answer in proportion to the speed error (a motor
          // that is not yet at its torque limit).
            Sim sim;
            sim.knobMS = knobs::speedLimitMS(3);
            sim.followGain = 1.0f;
            sim.rideLean();
            lean(600, 0);
            sim.run(1000);
            lean(600, 500);
            sim.run(300);
            assert((sim.last.left.value + sim.last.right.value) / 2 >= (sim.left + sim.right) / 2 + 30.0f); // still accelerating hard
            // ...and turning: about 47 deg/s here (the sideways-acceleration cap at this
            // speed), a 15 RPM difference between the commands. Limiting each wheel on
            // its own put both commands on the same limit: no difference at all.
            assert(sim.last.left.value - sim.last.right.value > 10.0f);
            assert(sim.left - sim.right > 2.0f);
            assert(std::fabs(sim.last.left.value - sim.left) <= MAX_RPM_COMMAND_DIFF + 0.01f);
            assert(std::fabs(sim.last.right.value - sim.right) <= MAX_RPM_COMMAND_DIFF + 0.01f);
        }
        { // A stop that begins mid-acceleration never speeds the chair up first.
            Sim sim;
            sluggish(sim);
            lean(600, 0);
            sim.run(1500);
            const float atStop = sim.left;
            sim.press(DISARM);
            float peak = atStop;
            for (int k = 0; k < 600; ++k)
            {
                sim.tick();
                peak = std::fmax(peak, sim.left);
                if (sim.last.left.kind == Kind::RPM)
                    assert(sim.last.left.value <= sim.left + 0.01f); // never commanded above the wheel
            }
            assert(peak <= atStop + 0.5f); // the three ticks it takes the press to register
            assert(std::fabs(sim.left) < STOP_DONE_RPM && !sim.drive.stopping());
        }
        { // The mirror: braking hard, then asking for speed again, picks up from the real speed.
            Sim sim;
            sluggish(sim);
            lean(600, 0);
            sim.run(5000);
            lean(0, 0);
            sim.run(600);
            assert(sim.last.left.value < sim.left - 20.0f); // the command fell faster than the wheels
            const float atLean = sim.left;
            lean(600, 0);
            float low = atLean;
            for (int k = 0; k < 200; ++k)
            {
                sim.tick();
                low = std::fmin(low, sim.left);
            }
            // It keeps slowing only until the lean (25 ms filter) asks for more than the
            // chair's real speed, about 80 ms here. Without the unwind it kept braking
            // for the half second the command needed to climb back up.
            assert(low > atLean - 5.0f);
        }
    }
    { // Partial lean. With the VESC's minimum ERPM lowered on the bench (here to
      // 3 wheel RPM) the chair's speed follows the lean all the way down.
        const float knee[2] = {settings::leanForewardBackResponse.get()[0], settings::leanForewardBackResponse.get()[1]};
        const float exponent = std::log(knee[1]) / std::log(knee[0]);
        const float deadband = settings::deadband.get();
        Sim sim;
        sim.vescMinRPM = 3.0f;
        assert(settings::vescMinSpeed.set(3.0f)); // the setting is kept equal to the VESCs'
        sim.drive.applySettings();
        assert(STOP_HANDOFF_RPM == 5.5f);
        sim.rideLean();
        messages.clear();
        const float top = sim.knobMS * LINEAR_TO_RPM;
        float previous = 0;
        for (int counts : {120, 180, 300, 450, 600}) // 4.5, 10, 25, 49, 78 RPM
        {
            lean(counts, 0);
            sim.run(2500);
            const float expected = std::pow((counts / 600.0f - deadband) / (1 - deadband), exponent) * top;
            assert(near(sim.left, expected, 0.5f) && near(sim.right, expected, 0.5f));
            assert(sim.left > previous + 2.0f); // every step of lean is a step of speed
            previous = sim.left;
        }
        assert(!logged("Hint:"));
        // The same on the spot: half a sideways lean is a proportionate turn rate.
        lean(0, 0);
        sim.run(2500);
        lean(0, 250);
        sim.run(1500);
        const float half = sim.left - sim.right;
        lean(0, 500);
        sim.run(1500);
        assert(half > 5.0f && sim.left - sim.right > 2.0f * half);
        // A stop now ramps the speed command nearly to rest before brake current takes over.
        lean(0, 0);
        sim.run(1500);
        lean(600, 0);
        sim.run(2500);
        sim.press(DISARM);
        float lowest = top;
        while (sim.last.left.kind == Kind::RPM)
        {
            lowest = std::fmin(lowest, sim.last.left.value);
            sim.tick();
        }
        assert(sim.last.left.kind == Kind::Brake && near(lowest, 5.5f, 1.0f));
        settings::vescMinSpeed.reset();
        sim.drive.applySettings();
        assert(STOP_HANDOFF_RPM == 18.0f);
        // With the VESC default (15.5 RPM) the same small leans do nothing, which
        // is the "nothing, nothing, then everything": the firmware cannot change
        // that setting, so it says so (next block).
        Sim stock;
        stock.rideLean();
        lean(120, 0);
        stock.run(2500);
        assert(stock.left == 0 && stock.last.left.kind == Kind::RPM && stock.last.left.value > 4.0f);
    }
    { // Braking with a back lean while steering: the chair turns the way the rider leans.
        Sim sim;
        sim.rideLean();
        lean(600, 0);
        sim.run(2500);
        lean(-240, 250); // lean back (60% reverse command) and right, at full forward speed
        bool wentLeft = false;
        float mostRight = 0;
        while (speedMS(sim) > 0.2f * sim.knobMS)
        {
            sim.tick();
            wentLeft = wentLeft || yawDegS(sim) < -1.0f;
            mostRight = std::fmax(mostRight, yawDegS(sim));
        }
        assert(!wentLeft && mostRight > 15.0f); // the old rule turned it left here, at up to 69 deg/s
        // The back lean is still held: the chair has braked to a standstill and stays
        // there. It does not carry on into reverse.
        float furthestBack = 0;
        for (int k = 0; k < 800; ++k)
        {
            sim.tick();
            furthestBack = std::fmin(furthestBack, std::fmin(sim.left, sim.right));
        }
        assert(furthestBack > -1.0f && std::fabs(sim.left) < 1.0f && std::fabs(sim.right) < 1.0f);
        // Back to neutral, then lean back again: now it reverses, at the reverse share of
        // the limit, and the team's rule applies (back and right swings the other way).
        lean(0, 0);
        sim.run(300);
        lean(-400, 250);
        sim.run(4000);
        assert(near(speedMS(sim), -REVERSE_SPEED_FRACTION * sim.knobMS, 0.02f));
        assert(yawDegS(sim) == 0.0f); // full back lean: steering off, as the team set it
        lean(-240, 250);
        sim.run(3000);
        assert(speedMS(sim) < -0.1f * sim.knobMS && yawDegS(sim) < -5.0f);
        // Reverse is never faster than its share of the limit, on any knob position.
        for (int position = 0; position < knobs::Positions; ++position)
        {
            Sim reverse;
            reverse.knobMS = knobs::speedLimitMS(position);
            reverse.rideLean();
            lean(-400, 0);
            float fastest = 0;
            for (int k = 0; k < 1200; ++k)
            {
                reverse.tick();
                fastest = std::fmin(fastest, std::fmin(reverse.last.left.value, reverse.left));
            }
            assert(fastest >= -REVERSE_SPEED_FRACTION * reverse.knobMS * LINEAR_TO_RPM - 0.5f);
            assert(fastest < -0.9f * REVERSE_SPEED_FRACTION * reverse.knobMS * LINEAR_TO_RPM);
        }
    }
    { // The startled rider: lean back AND to one side at speed, and hold it. The chair
      // steers the way of the lean while it brakes, stops, and then stays still. The
      // checks are on the commands sent, with a VESC that acts on small ones too.
        for (int side : {450, -450, 300, -150})
            for (int position : {1, 3})
            {
                Sim sim;
                sim.vescMinRPM = 3.0f;
                assert(settings::vescMinSpeed.set(3.0f));
                sim.drive.applySettings();
                sim.knobMS = knobs::speedLimitMS(position);
                sim.rideLean();
                lean(600, 0);
                sim.run(4000);
                lean(-300, side);
                float wrongWay = 0, turned = 0;
                for (int k = 0; k < 1600; ++k) // eight seconds
                {
                    sim.tick();
                    if (sim.last.left.kind == Kind::RPM && sim.last.right.kind == Kind::RPM)
                    { // the commanded turn is never against the lean
                        const float commanded = sim.last.left.value - sim.last.right.value;
                        wrongWay = std::fmax(wrongWay, side > 0 ? -commanded : commanded);
                    }
                    turned += (sim.left - sim.right) / LINEAR_TO_RPM / settings::trackWidth.get() * TickS * 180.0f / 3.14159265f;
                }
                assert(wrongWay < 0.5f); // it was a 23 RPM pivot the other way: 490 degrees in 5 s
                assert(turned * side >= -1.0f);
                assert(std::fabs(sim.left) < 1.0f && std::fabs(sim.right) < 1.0f); // standing still, lean still held
                assert(sim.last.left.kind != Kind::RPM || std::fabs(sim.last.left.value) < 0.5f);
                settings::vescMinSpeed.reset();
            }
    }
    { // Sitting up in the middle of a full-lean start: the chair stops speeding up at once,
      // even with a VESC that follows its speed command closely.
        for (uint32_t leanMs : {200u, 350u, 500u, 800u})
        {
            Sim sim;
            sim.vescMinRPM = 3.0f;
            sim.knobMS = knobs::speedLimitMS(3);
            sim.followGain = 200.0f; // tight tracking
            sim.followRPMS = 1000.0f;
            sim.rideLean();
            lean(600, 0);
            sim.run(leanMs);
            const float atRelease = sim.left;
            lean(0, 0);
            float peak = atRelease;
            for (int k = 0; k < 300; ++k)
            {
                sim.tick();
                peak = std::fmax(peak, sim.left);
            }
            // The lean filter (25 ms) keeps a little of the lean for a few ticks: under 0.15 m/s more.
            assert((peak - atRelease) / LINEAR_TO_RPM < 0.15f); // it was about 0.5 m/s
        }
    }
    { // A VESC with "Allow Braking" switched off does not slow the wheels under speed
      // commands. A stop notices, brakes by current, says so, and still meets Spec P.
        Sim sim;
        sim.vescAllowBraking = false;
        sim.knobMS = knobs::speedLimitMS(3);
        sim.rideLean();
        lean(600, 0);
        sim.run(4000);
        messages.clear();
        sim.press(DISARM);
        const uint32_t start = sim.now;
        while ((sim.drive.stopping() || std::fabs(sim.left) > STOP_DONE_RPM) && sim.now - start < 6000)
            sim.tick();
        assert(std::fabs(sim.left) <= STOP_DONE_RPM && sim.now - start < 3000); // Spec P: 3.0 s (it took 3.7 s)
        assert(logged("Allow Braking"));
        // Reported once, not at every stop.
        messages.clear();
        lean(0, 0);
        sim.press(LEAN);
        sim.pressCal();
        sim.run(1500);
        lean(600, 0);
        sim.run(3000);
        sim.press(DISARM);
        sim.run(4000);
        assert(!logged("Allow Braking") && std::fabs(sim.left) <= STOP_DONE_RPM);
        // A normal VESC never triggers it.
        Sim normal;
        normal.knobMS = knobs::speedLimitMS(3);
        normal.rideLean();
        lean(600, 0);
        normal.run(4000);
        messages.clear();
        normal.press(DISARM);
        normal.expectStop();
        assert(!logged("Allow Braking"));
    }
    { // Random rides. Whatever the rider does with their weight, some things must always hold.
        uint32_t seed = 12345;
        const auto random = [&seed](int low, int high) {
            seed = seed * 1664525u + 1013904223u;
            return low + static_cast<int>((seed >> 8) % static_cast<uint32_t>(high - low + 1));
        };
        for (int ride = 0; ride < 150; ++ride)
        {
            Sim sim;
            sim.vescMinRPM = ride % 2 ? 15.5f : 3.0f;
            assert(settings::vescMinSpeed.set(sim.vescMinRPM));
            sim.drive.applySettings();
            sim.knobMS = knobs::speedLimitMS(random(0, 3));
            if (ride % 3 == 0)
            { // a heavy, slow-to-respond chair
                sim.followRPMS = 60.0f;
            }
            sim.rideLean();
            const float top = sim.knobMS * LINEAR_TO_RPM;
            int forward = 0, side = 0;
            float neutralS = 0;
            for (int step = 0; step < 30; ++step)
            {
                // A new posture: often neutral, sometimes extreme.
                const int kind = random(0, 5);
                forward = kind == 0 ? 0 : kind == 1 ? -400 : random(-400, 600);
                side = kind == 0 ? 0 : random(-500, 500);
                lean(forward, side);
                const int ticks = random(40, 600);
                for (int k = 0; k < ticks; ++k)
                {
                    const float leftBefore = sim.left, rightBefore = sim.right;
                    const float meanBefore = (leftBefore + rightBefore) / 2;
                    sim.tick();
                    assert(sim.drive.mode() == LEAN && sim.drive.fault() == Fault::None);
                    const bool rpm = sim.last.left.kind == Kind::RPM && sim.last.right.kind == Kind::RPM;
                    // 1. No wheel is ever commanded past the speed-knob limit, forward or back.
                    if (sim.last.left.kind == Kind::RPM)
                        assert(std::fabs(sim.last.left.value) <= top + 0.01f);
                    if (sim.last.right.kind == Kind::RPM)
                        assert(std::fabs(sim.last.right.value) <= top + 0.01f);
                    // 2. Reverse never exceeds its share of the limit (plus the turn's half-difference).
                    if (rpm)
                        assert((sim.last.left.value + sim.last.right.value) / 2 >= -REVERSE_SPEED_FRACTION * top - 0.5f);
                    // 3. With no back lean, once the last posture's turn has died away (0.4 s),
                    //    the commanded turn is never against the rider's sideways lean. (A back
                    //    lean has its own rules and its own test above.)
                    if (rpm && forward >= 0 && k > 80)
                    {
                        const float turn = sim.last.left.value - sim.last.right.value;
                        if (side > 30)
                            assert(turn > -1.5f);
                        if (side < -30)
                            assert(turn < 1.5f);
                    }
                    // 4. A forward lean never drives the chair backwards. (Up to the 5 RPM unwind
                    //    margin of an old reverse command may remain for a moment; the wheels do
                    //    not move on it.)
                    if (rpm && forward > 60 && k > 20)
                        assert((sim.last.left.value + sim.last.right.value) / 2 > -MotionSmoother::LinearUnwindRPM - 0.5f || meanBefore < -1.0f);
                    if (forward > 60 && k > 100 && meanBefore >= -1.0f)
                        assert((sim.left + sim.right) / 2 > -1.5f);
                    // 5. Each wheel's speed command stays within 50 RPM of that wheel.
                    if (sim.last.left.kind == Kind::RPM)
                        assert(std::fabs(sim.last.left.value - leftBefore) <= MAX_RPM_COMMAND_DIFF + 0.01f);
                    if (sim.last.right.kind == Kind::RPM)
                        assert(std::fabs(sim.last.right.value - rightBefore) <= MAX_RPM_COMMAND_DIFF + 0.01f);
                }
                // 6. Neutral for three seconds: the chair is standing still.
                if (forward == 0 && side == 0)
                {
                    sim.run(3000);
                    neutralS += 3;
                    assert(std::fabs(sim.left) < 1.5f && std::fabs(sim.right) < 1.5f);
                }
            }
            (void)neutralS;
            // 7. DISARM from wherever the ride ended stops the chair.
            sim.press(DISARM);
            sim.run(3000);
            assert(std::fabs(sim.left) < STOP_DONE_RPM && std::fabs(sim.right) < STOP_DONE_RPM);
            settings::vescMinSpeed.reset();
        }
    }
    { // The panic gesture: full back lean at full speed, held. The chair stops and stays stopped.
        for (int position : {1, 3})
        {
            Sim sim;
            sim.knobMS = knobs::speedLimitMS(position);
            sim.rideLean();
            lean(600, 0);
            sim.run(4000);
            lean(-400, 0);
            float furthestBack = 0;
            for (int k = 0; k < 1600; ++k) // eight seconds
            {
                sim.tick();
                furthestBack = std::fmin(furthestBack, sim.left);
            }
            assert(furthestBack > -1.0f && std::fabs(sim.left) < 1.0f);
        }
    }
    { // Turning at the speed limit never moves forward speed faster than MAX_ACCEL.
        for (int position : {0, 3})
        {
            Sim sim;
            sim.knobMS = knobs::speedLimitMS(position);
            sim.rideLean();
            lean(600, 0);
            sim.run(4000);
            const float top = sim.knobMS * LINEAR_TO_RPM;
            assert(near(sim.last.left.value, top, 0.5f));
            float mean = (sim.last.left.value + sim.last.right.value) / 2;
            bool skip = false;
            for (int phase = 0; phase < 2; ++phase)
            {
                lean(600, phase == 0 ? 500 : 0); // full turn, then straight again
                for (int k = 0; k < 600; ++k)
                {
                    sim.tick();
                    const float next = (sim.last.left.value + sim.last.right.value) / 2;
                    // (Not while a wheel's command is at or inside the VESC's minimum-speed
                    // zone: on the slowest knob position the inner wheel goes through zero,
                    // and its command is held at the edge of that zone until the wheel is slow.)
                    const bool nearZero = std::fmin(std::fabs(sim.last.left.value), std::fabs(sim.last.right.value)) <= STOP_HANDOFF_RPM;
                    if (!nearZero && !skip)
                        assert(std::fabs(next - mean) <= MAX_ACCEL * LINEAR_TO_RPM * TickS + 0.01f);
                    skip = nearZero;
                    assert(std::fabs(sim.last.left.value) <= top + 0.01f && std::fabs(sim.last.right.value) <= top + 0.01f);
                    mean = next;
                }
            }
            assert(near(mean, top, 0.5f));
        }
    }
    { // Wheels held under a full lean (a curb, a helper's hands), then let go:
      // the command has not run away, so the chair picks up at the design rate.
        Sim sim;
        sim.knobMS = knobs::speedLimitMS(3);
        sim.rideLean();
        lean(600, 0);
        for (int k = 0; k < 600; ++k)
        {
            sim.left = sim.right = 0;
            sim.tick();
            assert(sim.last.left.value <= MAX_RPM_COMMAND_DIFF + 0.01f);
        }
        float lead = 0, previous = 0;
        for (int k = 0; k < 400; ++k)
        {
            sim.tick();
            if (k > 60) // after the first 0.3 s, in which the wheel closes the 50 RPM it was held back by
            {
                lead = std::fmax(lead, sim.last.left.value - sim.left);
                assert(sim.left - previous <= MAX_ACCEL * LINEAR_TO_RPM * TickS * 1.2f);
            }
            previous = sim.left;
        }
        assert(lead < 20.0f); // it used to stay about 48 RPM ahead all the way up
    }
    { // Power-on with the chair rolling (a reset or brown-out while driving): it is braked,
      // whatever caused the restart. The chair always starts in DISARM.
        for (bool watchdog : {false, true})
        {
            Sim sim(watchdog);
            assert(sim.drive.mode() == DISARM);
            assert(sim.drive.fault() == (watchdog ? Fault::WatchdogReset : Fault::None));
            sim.left = sim.right = 116.0f; // 3.7 m/s
            sim.tick();
            assert(sim.drive.stopping() && sim.last.left.kind == Kind::RPM && sim.last.left.value <= 116.0f);
            assert(sim.expectStop() < 1.5f && std::fabs(sim.left) < STOP_DONE_RPM);
            assert(sim.drive.fault() == (watchdog ? Fault::WatchdogReset : Fault::None)); // shown until a mode is armed
            // After a watchdog restart the chair is held, rider aboard or not, until DISARM is pressed.
            lean(0, 0);
            sim.run(400);
            sim.left = sim.right = 10.0f;
            sim.tick();
            assert((sim.last.left.kind == Kind::Brake) == watchdog);
            sim.left = sim.right = 0;
            sim.press(DISARM);
            sim.left = sim.right = 10.0f;
            sim.tick();
            assert(sim.last.left.kind == Kind::Current && sim.last.left.value == 0);
            sim.left = sim.right = 0;
            sim.run(1200);
            lean(0, 0);
            sim.run(400);
            sim.press(LEAN);
            assert(sim.drive.mode() == LEAN && sim.drive.fault() == Fault::None);
        }
        // Standing still: nothing to do.
        Sim still;
        still.run(1000);
        assert(!still.drive.stopping() && still.last.left.kind == Kind::Current && still.last.left.value == 0);
        // VESCs not reporting after the restart: brake blind after half a second.
        Sim silent;
        silent.leftAlive = silent.rightAlive = false;
        silent.left = silent.right = 70.0f;
        silent.run(495);
        assert(!silent.drive.stopping());
        silent.run(10);
        assert(silent.drive.stopping() && silent.last.left.kind == Kind::Brake);
        silent.run(STOP_BRAKE_MAX_MS + 20);
        assert(!silent.drive.stopping() && silent.last.left.kind == Kind::Current);
        // The recovery is decided once: a chair pushed along later in DISARM rolls freely.
        still.left = still.right = 30.0f;
        still.run(200);
        assert(!still.drive.stopping() && still.last.left.kind == Kind::Current && still.left > 25.0f);
        assert(std::string(DriveController::faultName(Fault::WatchdogReset)) == "WATCHDOG_RESET");
    }
    { // Sitting up really stops the chair, with the VESCs at their default
      // minimum speed (below 15.5 RPM the simulated VESC lets go of the motor,
      // as older VESC firmware does). The checks are on the wheels, not the commands.
        const float track = settings::trackWidth.get();
        const auto wheelYawDegS = [&](const Sim &sim) {
            return (sim.left - sim.right) / LINEAR_TO_RPM / track * 180.0f / 3.14159265f;
        };
        { // After a full-lean spin: the rotation is gone within a second and a few degrees.
            Sim sim;
            sim.rideLean();
            lean(0, 500);
            sim.run(2000);
            assert(near(wheelYawDegS(sim), 160.0f, 3.0f));
            lean(0, 0);
            float turned = 0;
            int ticks = 0;
            while (std::fabs(wheelYawDegS(sim)) > 5.0f && ticks < 2000)
            {
                sim.tick();
                turned += wheelYawDegS(sim) * TickS;
                ++ticks;
            }
            // 160 deg/s shed at 900 deg/s^2 is 14 degrees of command; the last part, on brake
            // current, takes this simulated chair to about 30 degrees in half a second.
            assert(ticks * TickS < 0.8f && turned < 40.0f); // it was 248 degrees over 4.5 s when the wheels coasted
            sim.run(1500);
            assert(std::fabs(sim.left) < STOP_DONE_RPM && std::fabs(sim.right) < STOP_DONE_RPM);
        }
        { // After full speed straight ahead: no long roll at the end.
            Sim sim;
            sim.rideLean();
            lean(600, 0);
            sim.run(2500);
            lean(0, 0);
            float rolled = 0, afterHandoff = 0;
            for (int k = 0; k < 800; ++k)
            {
                sim.tick();
                rolled += sim.left / LINEAR_TO_RPM * TickS;
                if (sim.left < STOP_HANDOFF_RPM)
                    afterHandoff += sim.left / LINEAR_TO_RPM * TickS;
            }
            assert(std::fabs(sim.left) < STOP_DONE_RPM);
            assert(afterHandoff < 0.25f); // it rolled 2.2 m from here when the wheels coasted
            assert(rolled < 2.0f);
        }
        { // On a slope at neutral lean the chair is held to a crawl instead of rolling away.
            Sim sim;
            sim.rideLean();
            sim.externalRPMS = 0.5f * LINEAR_TO_RPM; // a 5% slope
            float fastest = 0;
            for (int k = 0; k < 2000; ++k)
            {
                sim.tick();
                fastest = std::fmax(fastest, sim.left);
            }
            assert(fastest < 6.0f); // under 0.2 m/s; it reached 22 RPM (0.7 m/s) before
            // In DISARM the chair is a manual chair again and rolls freely.
            sim.press(DISARM);
            sim.run(2000);
            assert(sim.left > 20.0f);
        }
    }
    { // A chair that nothing is driving does not roll away. On a 1-in-12 ramp
      // (0.8 m/s^2) it is held to a crawl in every state except the ones meant
      // for moving it by hand.
        const float ramp = 0.8f * LINEAR_TO_RPM;
        const auto fastestOver = [](Sim &sim, uint32_t ms) {
            float fastest = 0;
            for (uint32_t t = 0; t < ms; t += TickMs)
            {
                sim.tick();
                fastest = std::fmax(fastest, std::fmax(std::fabs(sim.left), std::fabs(sim.right)));
            }
            return fastest;
        };
        const float crawl = 6.0f; // wheel RPM: under 0.2 m/s
        { // The rider gets out while driving in LEAN: the chair stops, and the empty chair stays put.
            Sim sim;
            sim.rideLean();
            lean(600, 0);
            sim.run(2500);
            sim.externalRPMS = ramp;
            loads(0, 65, 0, 0);
            sim.run(150);
            assert(sim.drive.mode() == DISARM);
            sim.expectStop();
            assert(fastestOver(sim, 8000) < crawl); // it reached 4.1 m/s (130 RPM) in 8 s before
            assert(sim.last.left.kind != Kind::RPM);
        }
        { // Parked in DISARM with the seat empty.
            Sim sim;
            loads(0, 65, 0, 0);
            sim.run(500);
            sim.externalRPMS = ramp;
            assert(fastestOver(sim, 5000) < crawl);
            // Tap DISARM to push the empty chair by hand: now it rolls freely.
            messages.clear();
            sim.press(DISARM);
            assert(sim.drive.freewheeling() && logged("Free-wheel"));
            assert(fastestOver(sim, 2000) > 40.0f);
            // Tap again and it is held again.
            sim.press(DISARM);
            assert(!sim.drive.freewheeling());
            sim.run(1500);
            assert(fastestOver(sim, 3000) < crawl);
            // Free-wheel also ends when someone sits down, and does not come back when they leave.
            sim.externalRPMS = 0;
            sim.press(DISARM);
            assert(sim.drive.freewheeling());
            lean(0, 0);
            sim.run(400);
            assert(!sim.drive.freewheeling());
            loads(0, 65, 0, 0);
            sim.run(300);
            sim.externalRPMS = ramp;
            assert(fastestOver(sim, 3000) < crawl);
        }
        { // DISARM with a rider in the seat is a manual chair: it rolls, the rider has the rims.
            Sim sim;
            lean(0, 0);
            sim.run(400);
            sim.externalRPMS = ramp;
            assert(fastestOver(sim, 3000) > 40.0f);
            assert(sim.last.left.kind == Kind::Current && sim.last.left.value == 0);
            assert(!sim.drive.freewheeling());
            sim.press(DISARM); // a tap with someone seated is not the free-wheel gesture
            assert(!sim.drive.freewheeling());
        }
        { // LEAN waiting for its calibration, and REMOTE waiting for a centred stick.
            Sim leanSim;
            lean(0, 0);
            leanSim.run(400);
            leanSim.press(LEAN);
            assert(leanSim.drive.mode() == LEAN && !leanSim.drive.lean().calibrated());
            leanSim.externalRPMS = ramp;
            assert(fastestOver(leanSim, 4000) < crawl);
            Sim remoteSim;
            remoteSim.run(400);
            remoteSim.press(REMOTE);
            assert(remoteSim.drive.mode() == REMOTE && remoteSim.drive.remoteStopped());
            remoteSim.externalRPMS = ramp;
            assert(fastestOver(remoteSim, 4000) < crawl);
            // Remote link lost while driving up the ramp: stops, then stays.
            remoteSim.externalRPMS = 0;
            remoteSim.runRemote(50, 0, 0);
            remoteSim.runRemote(2500, 1.0f, 1.0f);
            remoteSim.externalRPMS = -ramp;
            remoteSim.run(120);
            assert(remoteSim.drive.stopping());
            remoteSim.expectStop();
            assert(fastestOver(remoteSim, 5000) < crawl);
        }
        { // A VESC goes silent on the ramp with the seat empty (REMOTE): brake, and keep braking.
            Sim sim;
            lean(0, 0);
            sim.run(400);
            sim.press(REMOTE);
            sim.runRemote(50, 0, 0);
            loads(0, 65, 0, 0);
            sim.runRemote(2500, 1.0f, 1.0f);
            sim.externalRPMS = ramp;
            sim.leftAlive = false;
            sim.tick();
            assert(sim.drive.mode() == DISARM && sim.drive.fault() == Fault::VescSilent);
            sim.expectStop();
            for (int k = 0; k < 2000; ++k)
            { // ten seconds later it is still braking both wheels, not released
                sim.tick();
                assert(sim.last.left.kind == Kind::Brake && sim.last.left.value == STOP_BRAKE_AMPS);
                assert(sim.last.right.kind == Kind::Brake);
            }
            assert(std::fabs(sim.left) < crawl * 3); // brake current alone against the slope: slow, not free
        }
        { // A fault stop with the rider aboard: held until DISARM is pressed, then it is a manual chair.
            Sim sim;
            sim.rideLean();
            lean(600, 0);
            sim.run(2500);
            sim.externalRPMS = ramp;
            sensorInputs[PIN_B] = 4095; // a sensor shorts
            sim.run(30);
            assert(sim.drive.mode() == DISARM && sim.drive.fault() == Fault::SensorRail);
            sim.expectStop();
            assert(sim.drive.lean().isSeated());
            assert(fastestOver(sim, 5000) < crawl);
            sim.press(DISARM);
            assert(fastestOver(sim, 3000) > 40.0f);
            lean(0, 0);
        }
        { // Assist is for hand propulsion: no hold, and no boost from the slope either.
            Sim sim;
            lean(0, 0);
            sim.run(400);
            sim.hold(DISARM);
            sim.run(600);
            sim.release(DISARM);
            assert(sim.drive.mode() == ASSIST);
            sim.externalRPMS = ramp;
            for (int k = 0; k < 600; ++k)
            {
                sim.tick();
                assert(sim.last.left.kind == Kind::Current && sim.last.left.value == 0);
            }
            assert(sim.left > 40.0f);
        }
    }
    { // Standing at neutral, the neutral point follows slow sensor drift, so the
      // chair does not start creeping by itself after a long sit.
        Sim sim;
        sim.rideLean();
        // The back sensor creeps up 60 counts over five minutes (the empty-seat log
        // drifted about that much in an hour): three times the back deadband.
        for (int step = 0; step <= 60; ++step)
        {
            loads(NeutralF, NeutralB + step, NeutralL, NeutralR);
            sim.run(5000);
            assert(sim.last.left.kind == Kind::RPM && sim.last.left.value == 0 && sim.left == 0);
        }
        // A real lean still drives at once, measured from where neutral now is.
        loads(NeutralF + 300, NeutralB + 60, NeutralL, NeutralR);
        sim.run(1500);
        assert(sim.left > 10.0f);
        // The trim is bounded: a sensor that keeps climbing ends up as a lean, not as neutral.
        Sim runaway;
        runaway.rideLean();
        for (int step = 0; step <= 300; step += 5)
        {
            loads(NeutralF, NeutralB + step, NeutralL, NeutralR);
            runaway.run(5000);
        }
        // 300 counts on a 400-count range, 60 trimmed away at most: a reverse command.
        assert(runaway.last.left.kind == Kind::RPM && runaway.last.left.value < -5.0f);
        // It never trims while the rider is leaning or the chair is moving.
        Sim driving;
        driving.rideLean();
        lean(200, 0);
        driving.run(20000);
        const float cruising = driving.left;
        driving.run(20000);
        assert(near(driving.left, cruising, 0.2f));
    }
    { // A wheel that ignores a low speed command (VESC minimum ERPM) is pointed out, once.
        Sim sim;
        sim.rideLean();
        messages.clear();
        lean(120, 0); // a small lean: about 9 RPM on speed-knob position 2
        sim.run(1000);
        assert(sim.last.left.kind == Kind::RPM && sim.last.left.value > 4.0f && sim.last.left.value < 15.0f);
        assert(sim.left == 0 && !logged("Hint:")); // the simulated VESC lets go below 15.5 RPM
        sim.run(1500);
        assert(logged("lower s_pid_min_erpm"));
        messages.clear();
        sim.run(3000);
        assert(!logged("Hint:")); // not repeated
        // Not raised by normal driving, a standstill, or a wheel that does follow.
        Sim normal;
        normal.rideLean();
        messages.clear();
        normal.run(3000);
        lean(600, 0);
        normal.run(4000);
        lean(0, 0);
        normal.run(3000);
        assert(!logged("Hint:"));
    }
    { // Loading a profile needs a new calibration and starts from rest.
        Sim sim;
        sim.rideLean();
        sim.press(DISARM);
        sim.drive.resetForProfile();
        sim.press(LEAN);
        assert(sim.drive.mode() == LEAN && !sim.drive.lean().calibrated());
    }
    { // Random everything: leans, seat, buttons, remote, VESC dropouts, sensor shorts,
      // slopes, speed knob. Safety rules that must hold on every tick, whatever happens.
        uint32_t seed = 987654321;
        const auto random = [&seed](int low, int high) {
            seed = seed * 1664525u + 1013904223u;
            return low + static_cast<int>((seed >> 8) % static_cast<uint32_t>(high - low + 1));
        };
        const float fastest = MAX_SPEED * LINEAR_TO_RPM;
        // What the random rides actually reached, so the rules above are known to have been tried.
        long leanDriving = 0, remoteDriving = 0, assistTicks = 0, stoppingTicks = 0, faults = 0, braking = 0;
        for (int ride = 0; ride < 120; ++ride)
        {
            Sim sim;
            sim.vescMinRPM = ride % 2 ? 15.5f : 3.0f;
            assert(settings::vescMinSpeed.set(sim.vescMinRPM));
            sim.drive.applySettings();
            bool riderIn = false, remoteOn = false, shorted = false;
            float remoteL = 0, remoteR = 0;
            int forward = 0, side = 0;
            uint32_t emptySince = 0, silentSince = 0, shortedSince = 0;
            const auto posture = [&] {
                if (riderIn)
                    lean(forward, side);
                else
                    loads(0, 65, 0, 0);
                if (shorted)
                    sensorInputs[PIN_L] = 4095;
            };
            posture();
            for (int step = 0; step < 60; ++step)
            {
                switch (random(0, 13))
                {
                case 0: riderIn = !riderIn; if (!riderIn) emptySince = sim.now; break;
                case 1: case 2: forward = random(-400, 600); side = random(-500, 500); break;
                case 3: forward = side = 0; break;
                case 4: sim.pending.lean.edge = sim.pending.lean.pressed = true; break;
                case 5: sim.pending.remote.edge = sim.pending.remote.pressed = true; break;
                case 6: sim.pending.disarm.edge = sim.pending.disarm.pressed = true; break; // and held
                case 7: sim.pending.disarm.released = true; break;
                case 8: sim.pending.calibrate.pressed = true; sim.calHeld = random(0, 1); break;
                case 9: remoteOn = !remoteOn; remoteL = random(-100, 100) / 100.0f; remoteR = random(-100, 100) / 100.0f; break;
                case 10: remoteL = remoteR = 0; remoteOn = true; break;
                case 11:
                    if (random(0, 3) == 0)
                    {
                        sim.leftAlive = !sim.leftAlive;
                        if (!sim.leftAlive) silentSince = sim.now;
                    }
                    else
                        sim.leftAlive = true;
                    break;
                case 12:
                    shorted = random(0, 5) == 0;
                    if (shorted) shortedSince = sim.now;
                    break;
                default:
                    sim.knobMS = knobs::speedLimitMS(random(0, 3));
                    sim.externalRPMS = random(0, 4) == 0 ? random(-25, 25) : 0;
                    sim.calHeld = false;
                    break;
                }
                posture();
                const int ticks = random(10, 400);
                for (int k = 0; k < ticks; ++k)
                {
                    if (remoteOn && k % 2 == 0)
                        sim.remote(remoteL, remoteR);
                    sim.tick();
                    const auto mode = sim.drive.mode();
                    const auto &l = sim.last.left;
                    const auto &r = sim.last.right;
                    const bool moving = l.kind == Kind::RPM && std::fabs(l.value) > 5.0f;
                    leanDriving += mode == LEAN && moving;
                    remoteDriving += mode == REMOTE && moving;
                    assistTicks += mode == ASSIST;
                    stoppingTicks += sim.drive.stopping();
                    faults += sim.drive.fault() != Fault::None;
                    braking += l.kind == Kind::Brake;
                    // 1. Every command is a finite number inside its range.
                    for (const auto *w : {&l, &r})
                    {
                        assert(std::isfinite(w->value));
                        if (w->kind == Kind::RPM)
                            assert(std::fabs(w->value) <= fastest + 0.01f);
                        if (w->kind == Kind::Brake)
                            assert(w->value >= 0 && w->value <= STOP_BRAKE_AMPS);
                        if (w->kind == Kind::Current)
                            assert(std::fabs(w->value) <= settings::assistMaxBoostAmps.get());
                        // 2. Driving current only ever comes from Assist.
                        if (w->kind == Kind::Current && w->value != 0)
                            assert(mode == ASSIST);
                    }
                    // 3. LEAN and Assist never run with the seat empty for more than its hold time.
                    if (!riderIn && !shorted && sim.now - emptySince > 200)
                        assert(mode != LEAN && mode != ASSIST);
                    // 4. No powered mode without feedback from both VESCs, and no boost or
                    //    speed-up on stale speed.
                    if (!sim.leftAlive && sim.now - silentSince > 10)
                    {
                        assert(mode == DISARM);
                        assert(!(l.kind == Kind::Current && l.value != 0));
                    }
                    // 5. A shorted sensor ends LEAN and Assist within 30 ms.
                    if (shorted && sim.now - shortedSince > 30)
                        assert(mode != LEAN && mode != ASSIST);
                    // 6. In DISARM the only wheel-speed commands are a stop's falling ramp.
                    if (mode == DISARM && (l.kind == Kind::RPM || r.kind == Kind::RPM))
                        assert(sim.drive.stopping());
                    // 7. A stopped remote never drives.
                    if (mode == REMOTE && sim.drive.remoteStopped() && !sim.drive.stopping())
                        assert(l.kind != Kind::RPM && r.kind != Kind::RPM);
                    // 8. LEAN without a calibration never drives.
                    if (mode == LEAN && !sim.drive.lean().calibrated() && !sim.drive.stopping())
                        assert(l.kind != Kind::RPM && r.kind != Kind::RPM);
                }
            }
            // 9. From wherever that ended: level ground, DISARM, and the chair comes to rest.
            sim.externalRPMS = 0;
            sim.leftAlive = true;
            shorted = false;
            riderIn = true;
            forward = side = 0;
            posture();
            sim.pending = {};
            sim.calHeld = false;
            sim.run(50);
            sim.press(DISARM);
            sim.run(6000);
            assert(sim.drive.mode() == DISARM || sim.drive.mode() == ASSIST);
            assert(std::fabs(sim.left) < STOP_DONE_RPM && std::fabs(sim.right) < STOP_DONE_RPM);
            settings::vescMinSpeed.reset();
            settings::registry().defaults();
        }
        settings::persistence.refreshDirty();
        assert(leanDriving > 2000 && remoteDriving > 2000 && assistTicks > 2000);
        assert(stoppingTicks > 2000 && faults > 2000 && braking > 2000);
    }
    assert(std::string(DriveController::faultName(Fault::None)) == "NONE");
    assert(std::string(DriveController::faultName(Fault::SensorRail)) == "SENSOR_RAIL");
    assert(std::string(DriveController::faultName(Fault::VescSilent)) == "VESC_SILENT");
    puts("Drive controller ride simulations passed");
}
