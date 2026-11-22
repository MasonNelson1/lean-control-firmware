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
    bool calHeld = false;
    ModeStateMachine::Inputs pending;
    DriveController::MotorCommand last{Kind::Current, 0, 0};
    std::vector<DriveController::MotorCommand> trace;

    explicit Sim(mode startup = DISARM, bool afterWatchdogReset = false)
    {
        drive.begin(startup, afterWatchdogReset);
        drive.applySettings();
    }

    // A VESC in speed mode follows the command, but lets go of the motor below
    // its minimum ERPM (15.5 wheel RPM). Brake current slows the wheel. Zero
    // current coasts against a little friction; assist current accelerates.
    void wheel(float &rpm, Kind kind, float value) const
    {
        const float dt = TickS;
        const auto slow = [&](float rate) { rpm -= std::copysign(std::fmin(rate * dt, std::fabs(rpm)), rpm); };
        if (kind == Kind::RPM && std::fabs(value) >= vescMinRPM)
            rpm += std::fmin(followRPMS, std::fmax(-followRPMS, followGain * (value - rpm))) * dt;
        else if (kind == Kind::Brake)
            slow(100.0f);
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
        wheel(left, last.kind, last.left);
        wheel(right, last.kind, last.right);
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
        assert(drive.mode() == LEAN && last.kind == Kind::Current && last.left == 0);
        pressCal();
        run(1500);
        assert(drive.lean().calibrated());
    }
    // Checks a stop from here: wheel-speed commands only ever fall, then brake
    // current, then release. Returns the seconds until release.
    float expectStop(bool expectBrake = true)
    {
        float previous = std::fmax(std::fabs(left), std::fabs(right)) + MAX_RPM_COMMAND_DIFF;
        bool braked = false;
        const uint32_t start = now;
        for (int k = 0; k < 2000; ++k)
        {
            tick();
            if (last.kind == Kind::RPM)
            {
                assert(!braked);
                const float peak = std::fmax(std::fabs(last.left), std::fabs(last.right));
                assert(peak <= previous + 0.001f);
                previous = peak;
            }
            else if (last.kind == Kind::Brake)
            {
                braked = true;
                assert(last.left == STOP_BRAKE_AMPS && last.right == STOP_BRAKE_AMPS);
            }
            else
            {
                assert(last.left == 0 && last.right == 0);
                assert(braked == expectBrake);
                assert(!drive.stopping());
                return (now - start) / 1000.0f;
            }
        }
        assert(!"stop never finished");
        return 0;
    }
};

static float yawDegS(const Sim &sim)
{ // from the wheel-speed commands and the configured track width
    const float differenceMS = (sim.last.left - sim.last.right) / LINEAR_TO_RPM;
    return differenceMS / settings::trackWidth.get() * 180.0f / 3.14159265f;
}
static float speedMS(const Sim &sim) { return (sim.last.left + sim.last.right) / 2.0f / LINEAR_TO_RPM; }

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
        assert(sim.last.kind == Kind::Current && sim.last.left == 0 && sim.left == 0);
        lean(0, 0);
        sim.pressCal();
        sim.run(1500);
        assert(sim.drive.lean().calibrated());
        // Neutral: a zero wheel-speed command, and the chair has not moved.
        assert(sim.last.kind == Kind::RPM && sim.last.left == 0 && sim.last.right == 0 && sim.left == 0);
        lean(600, 0); // full forward
        sim.run(2500);
        const float top = sim.knobMS * LINEAR_TO_RPM;
        assert(sim.last.kind == Kind::RPM);
        assert(near(sim.last.left, top, 0.5f) && near(sim.last.right, top, 0.5f));
        assert(near(sim.left, top, 1.0f));
        assert(sim.drive.fault() == Fault::None);
    }
    { // Steering works while the chair is still accelerating, and stops when released.
        Sim sim;
        sim.rideLean();
        lean(600, 250);
        sim.run(250);
        // Forward speed is still building toward 78 RPM, but the turn is already there.
        assert(sim.last.kind == Kind::RPM);
        assert((sim.last.left + sim.last.right) / 2 < 60.0f);
        const float turning = yawDegS(sim);
        assert(turning > 20.0f);
        lean(600, 0); // straighten up, keep driving
        sim.run(250);
        assert(std::fabs(yawDegS(sim)) < 1.0f);
        assert((sim.last.left + sim.last.right) / 2 > 40.0f);
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
            assert(std::fabs(yawDegS(quick)) < 0.5f || quick.last.kind != Kind::RPM);
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
        assert(std::fabs(sim.last.left) <= sim.knobMS * LINEAR_TO_RPM + 0.01f);
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
            assert(sim.last.kind == Kind::RPM && sim.last.left < 78.0f); // already slowing on the first tick
            assert(sim.drive.stopping());
            const float seconds = sim.expectStop();
            // Ramp from 78 RPM at STOP_DECEL (0.53 s), then the full blind brake phase.
            assert(seconds > 0.5f && seconds < 0.6f + STOP_BRAKE_MAX_MS / 1000.0f);
            // Stays disarmed; a new selection is refused while the VESC is silent.
            messages.clear();
            sim.press(LEAN);
            sim.run(100);
            assert(sim.drive.mode() == DISARM && sim.drive.fault() == Fault::VescSilent);
            assert(sim.last.kind == Kind::Current && sim.last.left == 0);
            sim.press(REMOTE);
            sim.runRemote(200, 1, 1);
            assert(sim.drive.mode() == DISARM && sim.last.kind == Kind::Current);
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
        assert(sim.last.kind == Kind::Current && sim.left == 0);
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
        assert(sim.drive.mode() == DISARM && sim.last.kind == Kind::Current && sim.left == 0);
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
        assert(held.drive.mode() == DISARM && held.last.kind == Kind::Current && held.last.left == 0);
        // Assist needs its own press, starting from DISARM. If that comes while
        // the chair is still braking, the stop finishes before any boost.
        Sim again;
        again.knobMS = knobs::speedLimitMS(3);
        again.rideLean();
        lean(600, 0);
        again.run(3500);
        again.press(DISARM);
        again.hold(DISARM);
        again.run(520);
        assert(again.drive.mode() == ASSIST && again.drive.stopping() && again.left > 20.0f);
        again.expectStop();
        assert(again.drive.mode() == ASSIST && std::fabs(again.left) < STOP_DONE_RPM);

        Sim still;
        still.rideLean();
        still.press(DISARM);
        assert(still.drive.mode() == DISARM && !still.drive.stopping());
        assert(still.last.kind == Kind::Current && still.last.left == 0);
    }
    { // Pressing calibrate while rolling stops the chair first; DISARM works during calibration.
        Sim sim;
        sim.rideLean();
        lean(600, 0);
        sim.run(2500);
        sim.pending.calibrate.pressed = true;
        sim.calHeld = true;
        sim.tick();
        assert(sim.drive.lean().calibrationActive() && sim.drive.stopping());
        sim.run(40);
        sim.calHeld = false;
        // Neutral cannot be captured while the rider is still leaning and slowing down...
        const float seconds = sim.expectStop();
        assert(seconds < 1.0f);
        lean(0, 0);
        sim.run(1600);
        assert(sim.drive.lean().calibrated() && sim.drive.mode() == LEAN);
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
        assert(sim.last.kind == Kind::Current && sim.left == 0); // motors off throughout
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
        assert(sim.drive.mode() == DISARM && sim.last.kind == Kind::Current && sim.last.left == 0);

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
        assert(sim.last.kind == Kind::Current && sim.drive.remoteStopped());
        // A stick that is already held over when REMOTE is selected does nothing...
        sim.runRemote(1000, 1.0f, 1.0f);
        assert(sim.drive.remoteStopped() && sim.last.kind == Kind::Current && sim.left == 0);
        // ...until it has been centred once.
        sim.runRemote(30, 0.0f, 0.0f);
        assert(!sim.drive.remoteStopped());
        sim.runRemote(2500, 1.0f, 1.0f);
        const float top = sim.knobMS * LINEAR_TO_RPM;
        assert(near(sim.last.left, top, 0.5f) && !sim.drive.remoteStopped());
        // Controller lost: after the 100 ms timeout the chair brakes to a stop.
        sim.run(80); // the last packet was 10 ms before this
        assert(sim.last.kind == Kind::RPM && near(sim.last.left, top, 0.5f));
        sim.run(15);
        assert(sim.drive.remoteStopped() && sim.drive.stopping() && sim.drive.mode() == REMOTE);
        const float seconds = sim.expectStop();
        assert(seconds < 1.0f && std::fabs(sim.left) < STOP_DONE_RPM);

        // A short dropout: packets return mid-ramp and the command carries on from there.
        sim.runRemote(30, 0.0f, 0.0f); // (that stop outlasted half a second: centre the stick first)
        sim.runRemote(2500, 1.0f, 1.0f);
        sim.run(250);
        assert(sim.drive.stopping() && sim.last.kind == Kind::RPM);
        const float during = sim.last.left;
        assert(during < top - 10.0f && during > STOP_HANDOFF_RPM);
        sim.remote(1.0f, 1.0f);
        sim.tick();
        assert(!sim.drive.stopping() && sim.last.kind == Kind::RPM);
        assert(near(sim.last.left, during, 2.0f)); // no jump back to zero or up to full
        sim.runRemote(2000, 1.0f, 1.0f);
        assert(near(sim.last.left, top, 0.5f));

        // A real outage (3 s) with the stick held forward: the chair has stopped,
        // and the link coming back does not start it again.
        sim.run(3000);
        assert(std::fabs(sim.left) < STOP_DONE_RPM);
        sim.runRemote(1500, 1.0f, 1.0f);
        assert(sim.drive.remoteStopped() && sim.last.kind == Kind::Current && sim.left == 0);
        sim.runRemote(30, 0.0f, 0.0f); // stick centred
        sim.runRemote(2000, 1.0f, 1.0f);
        assert(near(sim.last.left, top, 0.5f));

        // Spin on the spot and tank steering still work through the two-axis smoother.
        sim.runRemote(2500, 0.5f, -0.5f);
        assert(near(sim.last.left, top / 2, 0.5f) && near(sim.last.right, -top / 2, 0.5f));
        sim.runRemote(2500, 1.0f, 0.4f);
        assert(near(sim.last.left, top, 0.5f) && near(sim.last.right, 0.4f * top, 0.5f));

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
    { // Engaging on a rolling chair starts from the wheels' speed instead of braking to zero.
        Sim sim;
        sim.rideRemote();
        sim.left = sim.right = 60.0f; // rolling
        sim.tick(); // stick centred, chair moving: stop under way
        sim.remote(1.0f, 1.0f);
        sim.tick();
        assert(sim.last.kind == Kind::RPM && near(sim.last.left, 60.0f, 3.0f));
        sim.runRemote(100, 1.0f, 1.0f);
        assert(sim.last.left > 60.0f);
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
            assert(sim.last.kind == Kind::RPM && std::fabs(sim.last.left) <= MAX_RPM_COMMAND_DIFF + 0.001f);
        }
    }
    { // ASSIST: boost fades out at the speed limit; DISARM releases; an empty seat brakes.
        Sim sim;
        lean(0, 0);
        sim.run(400);
        sim.hold(DISARM);
        sim.run(600); // held: Assist after half a second
        assert(sim.drive.mode() == ASSIST);
        sim.release(DISARM);
        const float limit = sim.knobMS * LINEAR_TO_RPM;
        // Pushed along at half the limit: boost in the direction of travel.
        float peak = 0;
        sim.left = sim.right = limit * 0.5f;
        for (int k = 0; k < 400; ++k)
        {
            sim.left = sim.right = limit * 0.5f;
            sim.tick();
            assert(sim.last.kind == Kind::Current && sim.last.left >= 0);
            peak = std::fmax(peak, sim.last.left);
        }
        assert(peak > 1.0f);
        // At and above the limit: no boost at all, so assist cannot run away.
        for (float fraction : {1.0f, 1.2f, 3.0f})
            for (int k = 0; k < 400; ++k)
            {
                sim.left = sim.right = limit * fraction;
                sim.tick();
                if (k > 100)
                    assert(sim.last.kind == Kind::Current && sim.last.left == 0 && sim.last.right == 0);
            }
        // Left to itself from a push, the chair never passes the limit.
        sim.left = sim.right = limit * 0.6f;
        float fastest = 0;
        for (int k = 0; k < 4000; ++k)
        {
            sim.tick();
            fastest = std::fmax(fastest, sim.left);
        }
        assert(fastest <= limit);

        // DISARM in Assist: motors released, no braking (the rider is pushing).
        sim.left = sim.right = 40.0f;
        sim.press(DISARM);
        assert(sim.drive.mode() == DISARM && !sim.drive.stopping());
        assert(sim.last.kind == Kind::Current && sim.last.left == 0);

        // Seat empty in Assist with the chair rolling: brake.
        sim.run(700);
        assert(sim.drive.mode() == DISARM); // a tap never reaches Assist
        sim.hold(DISARM);
        sim.run(600);
        assert(sim.drive.mode() == ASSIST);
        sim.release(DISARM);
        sim.left = sim.right = 40.0f;
        loads(0, 65, 0, 0);
        sim.run(120);
        assert(sim.drive.mode() == DISARM && sim.drive.stopping());
        sim.expectStop();
        // A silent VESC in Assist: disarmed, and no boost on stale speed.
        lean(0, 0);
        sim.run(400);
        assert(sim.drive.mode() == ASSIST); // seated again: Assist resumes by itself
        sim.leftAlive = false;
        sim.tick();
        assert(sim.drive.mode() == DISARM && sim.drive.fault() == Fault::VescSilent);
        assert(sim.last.kind == Kind::Current && sim.last.left == 0);
    }
    { // Turning the speed knob down at speed slows the chair at the design rate, not in a step.
        Sim sim;
        sim.knobMS = knobs::speedLimitMS(3);
        sim.rideLean();
        lean(600, 0);
        sim.run(3500);
        const float top = knobs::speedLimitMS(3) * LINEAR_TO_RPM;
        assert(near(sim.last.left, top, 0.5f));
        sim.knobMS = knobs::speedLimitMS(0);
        float previous = sim.last.left;
        for (int k = 0; k < 400; ++k)
        {
            sim.tick();
            assert(previous - sim.last.left <= MAX_ACCEL * LINEAR_TO_RPM * TickS + 0.01f);
            previous = sim.last.left;
        }
        assert(near(sim.last.left, knobs::speedLimitMS(0) * LINEAR_TO_RPM, 0.5f));
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
            assert(sim.last.left > sim.left + 40.0f); // the command is well ahead of the wheel
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
                assert(sim.last.kind == Kind::RPM && sim.last.left >= STOP_HANDOFF_RPM);
                assert(sim.left < before); // braked all the way down, never coasting
            }
            assert(sim.left <= STOP_HANDOFF_RPM + 4.0f);
            sim.run(500);
            assert(std::fabs(sim.last.left) < 1.0f); // then the command goes to zero as before
        }
        { // The same for a turn: straightening up stops the rotation building.
            Sim sim;
            sluggish(sim);
            lean(0, 500);
            sim.run(400);
            const float atRelease = sim.left - sim.right;
            assert(atRelease > 10.0f && (sim.last.left - sim.last.right) > atRelease + 4.0f);
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
            assert((sim.last.left + sim.last.right) / 2 >= (sim.left + sim.right) / 2 + 30.0f); // still accelerating hard
            // ...and turning: about 47 deg/s here (the sideways-acceleration cap at this
            // speed), a 15 RPM difference between the commands. Limiting each wheel on
            // its own put both commands on the same limit: no difference at all.
            assert(sim.last.left - sim.last.right > 10.0f);
            assert(sim.left - sim.right > 2.0f);
            assert(std::fabs(sim.last.left - sim.left) <= MAX_RPM_COMMAND_DIFF + 0.01f);
            assert(std::fabs(sim.last.right - sim.right) <= MAX_RPM_COMMAND_DIFF + 0.01f);
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
                if (sim.last.kind == Kind::RPM)
                    assert(sim.last.left <= sim.left + 0.01f); // never commanded above the wheel
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
            assert(sim.last.left < sim.left - 20.0f); // the command fell faster than the wheels
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
        while (sim.last.kind == Kind::RPM)
        {
            lowest = std::fmin(lowest, sim.last.left);
            sim.tick();
        }
        assert(sim.last.kind == Kind::Brake && near(lowest, 5.5f, 1.0f));
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
        assert(stock.left == 0 && stock.last.kind == Kind::RPM && stock.last.left > 4.0f);
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
        // Once it is reversing, the team's rule applies: back and right swings the other way.
        sim.run(3000);
        assert(speedMS(sim) < -0.3f * sim.knobMS && yawDegS(sim) < -5.0f);
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
            assert(near(sim.last.left, top, 0.5f));
            float mean = (sim.last.left + sim.last.right) / 2;
            bool skip = false;
            for (int phase = 0; phase < 2; ++phase)
            {
                lean(600, phase == 0 ? 500 : 0); // full turn, then straight again
                for (int k = 0; k < 600; ++k)
                {
                    sim.tick();
                    const float next = (sim.last.left + sim.last.right) / 2;
                    // (Not while a wheel's command is at or inside the VESC's minimum-speed
                    // zone: on the slowest knob position the inner wheel goes through zero,
                    // and its command is held at the edge of that zone until the wheel is slow.)
                    const bool nearZero = std::fmin(std::fabs(sim.last.left), std::fabs(sim.last.right)) <= STOP_HANDOFF_RPM;
                    if (!nearZero && !skip)
                        assert(std::fabs(next - mean) <= MAX_ACCEL * LINEAR_TO_RPM * TickS + 0.01f);
                    skip = nearZero;
                    assert(std::fabs(sim.last.left) <= top + 0.01f && std::fabs(sim.last.right) <= top + 0.01f);
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
            assert(sim.last.left <= MAX_RPM_COMMAND_DIFF + 0.01f);
        }
        float lead = 0, previous = 0;
        for (int k = 0; k < 400; ++k)
        {
            sim.tick();
            if (k > 60) // after the first 0.3 s, in which the wheel closes the 50 RPM it was held back by
            {
                lead = std::fmax(lead, sim.last.left - sim.left);
                assert(sim.left - previous <= MAX_ACCEL * LINEAR_TO_RPM * TickS * 1.2f);
            }
            previous = sim.left;
        }
        assert(lead < 20.0f); // it used to stay about 48 RPM ahead all the way up
    }
    { // After a watchdog restart: disarmed whatever the startup mode, and a rolling chair is braked.
        Sim sim(REMOTE, /*afterWatchdogReset=*/true);
        assert(sim.drive.mode() == DISARM && sim.drive.fault() == Fault::WatchdogReset);
        sim.left = sim.right = 70.0f;
        sim.tick();
        assert(sim.drive.stopping() && sim.last.kind == Kind::RPM && sim.last.left <= 70.0f);
        assert(sim.expectStop() < 1.0f && std::fabs(sim.left) < STOP_DONE_RPM);
        assert(sim.drive.fault() == Fault::WatchdogReset); // shown until a mode is armed again
        lean(0, 0);
        sim.run(400);
        sim.press(LEAN);
        assert(sim.drive.mode() == LEAN && sim.drive.fault() == Fault::None);
        // Standing still: nothing to do.
        Sim still(DISARM, true);
        still.run(1000);
        assert(!still.drive.stopping() && still.last.kind == Kind::Current && still.last.left == 0);
        // VESCs not reporting after the restart: brake blind after half a second.
        Sim silent(DISARM, true);
        silent.leftAlive = silent.rightAlive = false;
        silent.left = silent.right = 70.0f;
        silent.run(495);
        assert(!silent.drive.stopping());
        silent.run(10);
        assert(silent.drive.stopping() && silent.last.kind == Kind::Brake);
        silent.run(STOP_BRAKE_MAX_MS + 20);
        assert(!silent.drive.stopping() && silent.last.kind == Kind::Current);
        // A normal power-on honours the startup mode and brakes nothing.
        Sim normal(REMOTE, false);
        assert(normal.drive.mode() == REMOTE && normal.drive.fault() == Fault::None);
        assert(std::string(DriveController::faultName(Fault::WatchdogReset)) == "WATCHDOG_RESET");
    }
    { // A wheel that ignores a low speed command (VESC minimum ERPM) is pointed out, once.
        Sim sim;
        sim.rideLean();
        messages.clear();
        lean(120, 0); // a small lean: about 9 RPM on speed-knob position 2
        sim.run(1000);
        assert(sim.last.kind == Kind::RPM && sim.last.left > 4.0f && sim.last.left < 15.0f);
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
    assert(std::string(DriveController::faultName(Fault::None)) == "NONE");
    assert(std::string(DriveController::faultName(Fault::SensorRail)) == "SENSOR_RAIL");
    assert(std::string(DriveController::faultName(Fault::VescSilent)) == "VESC_SILENT");
    puts("Drive controller ride simulations passed");
}
