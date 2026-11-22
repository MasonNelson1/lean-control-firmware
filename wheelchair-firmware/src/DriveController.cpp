#include "DriveController.h"

#include <algorithm>
#include <cmath>

#include "config/DriveSettings.h"
#include "drive_knobs.h"
#include "telemetry/Telemetry.h"

using namespace common;

namespace
{
    constexpr float DegToRad = 3.14159265f / 180.0f;

    float clampTo(float value, float low, float high) { return std::min(high, std::max(low, value)); }
} // namespace

const char *DriveController::faultName(Fault fault)
{
    switch (fault)
    {
    case Fault::SensorRail: return "SENSOR_RAIL";
    case Fault::VescSilent: return "VESC_SILENT";
    default: return "NONE";
    }
}

void DriveController::begin(common::mode initial)
{
    modeState_.begin(initial);
    lean_.reset();
    remote_.reset();
    assist_.reset();
    motion_.reset();
    stop_.cancel();
    driving_ = false;
    fault_ = Fault::None;
}

void DriveController::applySettings()
{
    lean_.updateResponseCurve();
    // setTuning() preserves the existing motion and error history.
    motion_.setTuning(settings::normalPid.tuning());
}

void DriveController::resetForProfile()
{
    lean_.reset();
    assist_.reset();
    motion_.reset();
    driving_ = false;
    commands_ = {};
}

ModeStateMachine::Actions DriveController::updateButtons(ModeStateMachine::Inputs input, uint32_t now)
{
    input.seated = lean_.isSeated();
    input.calibrationActive = lean_.calibrationActive();
    calPressed_ |= input.calibrate.pressed;
    const auto actions = modeState_.update(input, now);
    if (actions.resetRemote)
        remote_.reset();
    if (actions.resetAssist)
        assist_.reset();
    return actions;
}

void DriveController::raiseFault(Fault fault)
{
    fault_ = fault;
    telemetry::Telemetry::logf(fault == Fault::SensorRail
                                   ? "Fault: a seat sensor is reading at an ADC rail (shorted or unplugged). Disarming.\n"
                                   : "Fault: a motor controller stopped reporting its speed. Disarming.\n");
    modeState_.requestDisarm();
}

// Start bringing the chair to rest, unless it already is at rest.
void DriveController::beginStop(const TickInput &in, bool fromMeasured)
{
    const bool fresh = in.leftFresh && in.rightFresh;
    const float measured = std::max(std::fabs(in.leftRPM), std::fabs(in.rightRPM));
    const float commanded = std::max(std::fabs(sentLeftRPM_), std::fabs(sentRightRPM_));
    if (fromMeasured)
    {
        // No speed command was active (Assist drives by current).
        if (!fresh)
            stop_.beginBlind(in.now);
        else if (measured >= STOP_DONE_RPM)
            stop_.begin(in.leftRPM, in.rightRPM, in.now);
        return;
    }
    if (fresh && measured < STOP_DONE_RPM && commanded < STOP_DONE_RPM)
        return; // standing still
    // Ramp from the commands being sent. They are within MAX_RPM_COMMAND_DIFF
    // of the wheels, and they are still right when a VESC has gone silent.
    stop_.begin(sentLeftRPM_, sentRightRPM_, in.now);
}

void DriveController::applySeat(const TickInput &in)
{
    const common::mode previous = modeState_.current();
    const auto transition = modeState_.applySeat(lean_.isSeated(), in.now);
    if (!transition.changed)
        return;

    lean_.cancelCalibration(); // also ends a tare when leaving DISARM
    if (modeState_.current() == DISARM)
    {
        if (previous == LEAN || previous == REMOTE)
        {
            if (driving_)
                beginStop(in, /*fromMeasured=*/false);
        }
        else if (previous == ASSIST && transition.seatDisarm)
        {
            // The rider left a moving chair. A DISARM press in Assist only
            // releases the motors: the rider is propelling by hand.
            beginStop(in, /*fromMeasured=*/true);
        }
        driving_ = false;
        motion_.reset();
        assist_.reset();
    }
    if (transition.resetLean)
        lean_.reset();
    telemetry::Telemetry::logf("Mode: %s\n", modeName(modeState_.current()));
}

DriveController::MotorCommand DriveController::stopOrRelease(const TickInput &in)
{
    if (driving_)
    {
        // The drive source went away while wheel-speed commands were active.
        beginStop(in, /*fromMeasured=*/false);
        driving_ = false;
        motion_.reset();
    }
    commands_ = {};
    sentLeftRPM_ = sentRightRPM_ = 0.0f;
    if (!stop_.active())
        return {MotorCommand::Kind::Current, 0.0f, 0.0f};

    const bool fresh = in.leftFresh && in.rightFresh;
    const auto out = stop_.update(in.dt, in.now, {in.leftRPM, in.rightRPM, fresh});
    switch (out.drive)
    {
    case StopController::Drive::RPM:
    {
        float left = out.left, right = out.right;
        if (fresh)
        {
            // Same envelope as normal driving, so a stop cannot ask the VESC
            // for a bigger speed step than driving ever does.
            left = clampTo(left, in.leftRPM - MAX_RPM_COMMAND_DIFF, in.leftRPM + MAX_RPM_COMMAND_DIFF);
            right = clampTo(right, in.rightRPM - MAX_RPM_COMMAND_DIFF, in.rightRPM + MAX_RPM_COMMAND_DIFF);
        }
        commands_ = {0.0f, 0.0f, left, right};
        sentLeftRPM_ = left;
        sentRightRPM_ = right;
        return {MotorCommand::Kind::RPM, left, right};
    }
    case StopController::Drive::Brake:
        return {MotorCommand::Kind::Brake, out.left, out.right};
    default:
        return {MotorCommand::Kind::Current, 0.0f, 0.0f};
    }
}

DriveController::MotorCommand DriveController::driveRPM(const TickInput &in, float linearTargetRPM, float turnTargetRPM)
{
    const bool fresh = in.leftFresh && in.rightFresh;
    if (stop_.active())
    {
        // Drive came back mid-stop (e.g. a short remote dropout): carry on
        // from where the stop had got to instead of from zero.
        if (stop_.phase() == StopController::Phase::Ramp)
            motion_.seed(stop_.leftRPM(), stop_.rightRPM());
        else if (fresh)
            motion_.seed(in.leftRPM, in.rightRPM);
        else
            motion_.reset();
        stop_.cancel();
        driving_ = true;
    }
    else if (!driving_)
    {
        // Engaging while the chair rolls: start from the wheels' real speed,
        // so the command does not restart at zero and brake hard.
        if (fresh)
            motion_.seed(in.leftRPM, in.rightRPM);
        else
            motion_.reset();
        driving_ = true;
    }

    const float halfTrackRPM = settings::trackWidth.get() / 2.0f * LINEAR_TO_RPM; // wheel RPM per rad/s of turn
    const auto wheels = motion_.update(linearTargetRPM, turnTargetRPM, in.dt, speedLimitMS_,
                                       TURN_ACCEL_DEG_S2 * DegToRad * halfTrackRPM,
                                       TURN_DECEL_DEG_S2 * DegToRad * halfTrackRPM);
    float left = wheels.left, right = wheels.right;
    if (fresh)
    {
        left = clampTo(left, in.leftRPM - MAX_RPM_COMMAND_DIFF, in.leftRPM + MAX_RPM_COMMAND_DIFF);
        right = clampTo(right, in.rightRPM - MAX_RPM_COMMAND_DIFF, in.rightRPM + MAX_RPM_COMMAND_DIFF);
    }
    commands_ = {linearTargetRPM + turnTargetRPM, linearTargetRPM - turnTargetRPM, left, right};
    sentLeftRPM_ = left;
    sentRightRPM_ = right;
    return {MotorCommand::Kind::RPM, left, right};
}

void DriveController::saveCalibration()
{
    if (!settings::persistence.saveCalibration())
        telemetry::Telemetry::logf("Could not save the calibration to EEPROM; it will be lost at power-off.\n");
}

DriveController::MotorCommand DriveController::tick(const TickInput &in)
{
    lean_.sample(in.dt);
    const bool calPressed = calPressed_;
    calPressed_ = false;
    const bool calibrationRequested = modeState_.takeCalibrationRequest();
    const bool fresh = in.leftFresh && in.rightFresh;
    // Turning the speed knob up takes effect at once (the smoother still limits
    // acceleration). Turning it down lowers the cap no faster than the design
    // braking rate, so the smoother never sees a step drop in its speed limit.
    speedLimitMS_ = knobs::slewDown(speedLimitMS_, in.speedLimitMS, MAX_ACCEL, in.dt);

    // ---- Faults: checked whenever a mode is armed or being armed ----------
    const common::mode selected = modeState_.selected();
    if (selected != DISARM)
    {
        if ((selected == LEAN || selected == ASSIST) && lean_.sensorAtRail())
            raiseFault(Fault::SensorRail); // the seat reading cannot be trusted either
        else if (!fresh)
            raiseFault(Fault::VescSilent); // every drive mode needs wheel-speed feedback
        else
            fault_ = Fault::None;
    }

    // ---- Calibration button -------------------------------------------------
    bool calibrationBegan = false;
    if (calibrationRequested && modeState_.current() == LEAN && lean_.isSeated() && !lean_.calibrationActive())
    {
        lean_.beginCalibration(in.now);
        calibrationBegan = true;
    }
    if (calPressed && modeState_.current() == DISARM && modeState_.selected() == DISARM &&
        !stop_.active() && !lean_.tareActive())
        lean_.beginTare(in.now);

    applySeat(in);

    assistCurrent_ = {};
    switch (modeState_.current())
    {
    case LEAN:
    {
        if (lean_.calibrationActive())
        {
            const auto event = lean_.updateCalibration(in.now, in.calButtonHeld, calPressed && !calibrationBegan);
            if (event == LeanDriveMode::Event::RangeDone)
                saveCalibration();
            if (event == LeanDriveMode::Event::RangeDone || event == LeanDriveMode::Event::RangeRejected)
            {
                // The rider may be mid-lean. Require a fresh mode selection.
                modeState_.requestDisarm();
                applySeat(in);
                return stopOrRelease(in);
            }
        }
        const auto command = lean_.command();
        if (command.stopped)
            return stopOrRelease(in);

        const float linearTargetRPM = command.speed * speedLimitMS_ * LINEAR_TO_RPM;
        // Turn is a rate in deg/s, not a share of the speed knob. At speed it
        // is capped so that speed x turn rate (sideways acceleration) stays
        // under the limit. The speed used is the faster of what is commanded
        // and what the wheels report.
        const float speedMS = std::max(std::fabs(motion_.linearRPM()),
                                       std::fabs(in.leftRPM + in.rightRPM) / 2.0f) / LINEAR_TO_RPM;
        float turnRate = settings::maxTurnRate.get() * DegToRad;
        const float lateral = settings::maxLateralAccel.get();
        if (speedMS * turnRate > lateral)
            turnRate = lateral / speedMS;
        const float turnTargetRPM = command.turn * turnRate * settings::trackWidth.get() / 2.0f * LINEAR_TO_RPM;
        return driveRPM(in, linearTargetRPM, turnTargetRPM);
    }

    case REMOTE:
    {
        const auto targets = remote_.getTargets(in.now);
        if (remote_.stopped())
            return stopOrRelease(in);
        const float maxRPM = speedLimitMS_ * LINEAR_TO_RPM;
        const float left = clampTo(targets.left, -1.0f, 1.0f) * maxRPM;
        const float right = clampTo(targets.right, -1.0f, 1.0f) * maxRPM;
        return driveRPM(in, (left + right) / 2.0f, (left - right) / 2.0f);
    }

    case ASSIST:
    {
        // Holding DISARM at speed reaches Assist while the chair is still
        // braking. Finish the stop first; boost starts once it is at rest.
        if (driving_ || stop_.active())
            return stopOrRelease(in);
        commands_ = {};
        sentLeftRPM_ = sentRightRPM_ = 0.0f;
        assistCurrent_ = assist_.getTargets(in.leftRPM, in.rightRPM, in.speedLimitMS * LINEAR_TO_RPM, in.now);
        return {MotorCommand::Kind::Current, assistCurrent_.left, assistCurrent_.right};
    }

    default: // DISARM
    {
        if (lean_.tareActive() && lean_.updateTare(in.now, in.calButtonHeld) == LeanDriveMode::Event::TareDone)
            saveCalibration();
        return stopOrRelease(in);
    }
    }
}
