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
    bool calHeld = false;
    ModeStateMachine::Inputs pending;
    DriveController::MotorCommand last{Kind::Current, 0, 0};
    std::vector<DriveController::MotorCommand> trace;

    Sim()
    {
        drive.begin(DISARM);
        drive.applySettings();
    }

    // A VESC in speed mode follows the command, but lets go of the motor below
    // its minimum ERPM (15.5 wheel RPM). Brake current slows the wheel. Zero
    // current coasts against a little friction; assist current accelerates.
    static void wheel(float &rpm, Kind kind, float value)
    {
        const float dt = TickS;
        const auto slow = [&](float rate) { rpm -= std::copysign(std::fmin(rate * dt, std::fabs(rpm)), rpm); };
        if (kind == Kind::RPM && std::fabs(value) >= 15.5f)
            rpm += std::fmin(300.0f * dt, std::fmax(-300.0f * dt, value - rpm));
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

        // Holding DISARM at speed reaches Assist after half a second, while
        // the chair is still braking: the stop finishes before any boost.
        Sim held;
        held.knobMS = knobs::speedLimitMS(3);
        held.rideLean();
        lean(600, 0);
        held.run(3500);
        held.hold(DISARM);
        held.run(600);
        assert(held.drive.mode() == ASSIST && held.drive.stopping());
        assert(held.last.kind == Kind::RPM && held.left > 30.0f);
        held.expectStop();
        assert(held.drive.mode() == ASSIST && std::fabs(held.left) < STOP_DONE_RPM);

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
        assert(settings::persistence.save() || true);
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
    }
    { // Engaging on a rolling chair starts from the wheels' speed instead of braking to zero.
        Sim sim;
        lean(0, 0);
        sim.run(400);
        sim.left = sim.right = 60.0f; // being pushed along
        sim.press(REMOTE);
        sim.remote(1.0f, 1.0f);
        sim.tick();
        assert(sim.last.kind == Kind::RPM && near(sim.last.left, 60.0f, 3.0f));
        sim.runRemote(100, 1.0f, 1.0f);
        assert(sim.last.left > 60.0f);
    }
    { // The speed command never strays more than 50 RPM from the wheel.
        Sim sim;
        lean(0, 0);
        sim.run(400);
        sim.press(REMOTE);
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
