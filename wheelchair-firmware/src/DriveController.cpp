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
    case Fault::WatchdogReset: return "WATCHDOG_RESET";
    default: return "NONE";
    }
}

void DriveController::begin(common::mode initial, bool afterWatchdogReset)
{
    modeState_.begin(afterWatchdogReset ? DISARM : initial);
    recoverFromReset_ = afterWatchdogReset;
    recoverTimed_ = false;
    lean_.reset();
    remote_.reset();
    assist_.reset();
    motion_.reset();
    stop_.cancel();
    driving_ = false;
    fault_ = afterWatchdogReset ? Fault::WatchdogReset : Fault::None;
}

void DriveController::applySettings()
{
    lean_.updateResponseCurve();
    STOP_HANDOFF_RPM = settings::vescMinSpeed.get() + STOP_HANDOFF_MARGIN_RPM;
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
    // An unplugged sensor does not show up here: the sensors are two-wire,
    // so an open circuit reads the same as an unloaded sensor. (LeanDriveMode
    // makes sure that cannot create motion.)
    telemetry::Telemetry::logf(fault == Fault::SensorRail
                                   ? "Fault: a seat sensor is reading at an ADC rail (shorted wiring or a failed amplifier). Disarming.\n"
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
    if (!fresh)
    {
        // A VESC has gone silent: the commands being sent are the best
        // knowledge of the wheel speeds (within MAX_RPM_COMMAND_DIFF of them).
        stop_.begin(sentLeftRPM_, sentRightRPM_, in.now);
        return;
    }
    if (measured < STOP_DONE_RPM && commanded < STOP_DONE_RPM)
        return; // standing still
    // Ramp from the commands being sent, but never from above the wheel's real
    // speed: while the chair is accelerating the command leads the wheel, and
    // a stop that started from the command would keep speeding up at first.
    const auto start = [](float sent, float wheel) {
        return sent >= 0.0f ? std::min(sent, std::max(wheel, 0.0f)) : std::max(sent, std::min(wheel, 0.0f));
    };
    stop_.begin(start(sentLeftRPM_, in.leftRPM), start(sentRightRPM_, in.rightRPM), in.now);
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

    const float measuredLinear = (in.leftRPM + in.rightRPM) / 2.0f;
    const float measuredTurn = (in.leftRPM - in.rightRPM) / 2.0f;
    if (fresh)
        motion_.unwind(measuredLinear, measuredTurn, linearTargetRPM, turnTargetRPM);

    const float halfTrackRPM = settings::trackWidth.get() / 2.0f * LINEAR_TO_RPM; // wheel RPM per rad/s of turn
    const auto wheels = motion_.update(linearTargetRPM, turnTargetRPM, in.dt, speedLimitMS_,
                                       TURN_ACCEL_DEG_S2 * DegToRad * halfTrackRPM,
                                       TURN_DECEL_DEG_S2 * DegToRad * halfTrackRPM);
    float left = wheels.left, right = wheels.right;
    if (fresh)
    {
        // Keep each wheel's command within MAX_RPM_COMMAND_DIFF of its measured
        // speed. The allowance goes to the turn first and the rest to forward
        // speed. Limiting each wheel on its own put both commands on the same
        // limit whenever the forward command was far ahead of the wheels, and
        // the turn dropped out of what was sent.
        const float turnError = clampTo((left - right) / 2.0f - measuredTurn, -MAX_RPM_COMMAND_DIFF, MAX_RPM_COMMAND_DIFF);
        const float room = MAX_RPM_COMMAND_DIFF - std::fabs(turnError);
        const float linearError = clampTo((left + right) / 2.0f - measuredLinear, -room, room);
        left = measuredLinear + linearError + measuredTurn + turnError;
        right = measuredLinear + linearError - measuredTurn - turnError;
        // A VESC in speed mode releases the motor when the command is under
        // its minimum ERPM. If the command gets there while the wheel is
        // still well above it (the wheel lags a braking command), the chair
        // would coast from that speed. Hold the command at the edge until the
        // wheel has come down, so the VESC keeps braking.
        const auto keepBraking = [](float command, float wheel) {
            return std::fabs(command) < STOP_HANDOFF_RPM && std::fabs(wheel) > STOP_HANDOFF_RPM + 4.0f
                       ? std::copysign(STOP_HANDOFF_RPM, wheel)
                       : command;
        };
        left = keepBraking(left, in.leftRPM);
        right = keepBraking(right, in.rightRPM);
        // The command itself may not run further ahead than what is sent.
        motion_.limitLead(measuredLinear, measuredTurn, MAX_RPM_COMMAND_DIFF);
    }
    commands_ = {linearTargetRPM + turnTargetRPM, linearTargetRPM - turnTargetRPM, left, right};
    sentLeftRPM_ = left;
    sentRightRPM_ = right;
    if (fresh)
        watchLowSpeed(in, left, right);
    return {MotorCommand::Kind::RPM, left, right};
}

// Diagnostic only. A VESC in speed mode ignores commands below its minimum
// ERPM (VESC Tool default 900 = 15.5 wheel RPM): a small lean does nothing,
// then the chair starts at walking pace all at once. The firmware cannot
// change that setting, but it can notice a wheel that sits still under a
// steady low command and say so once.
void DriveController::watchLowSpeed(const TickInput &in, float left, float right)
{
    const auto ignored = [](float command, float measured) {
        return std::fabs(command) >= 4.0f && std::fabs(command) < STOP_HANDOFF_RPM && std::fabs(measured) < 1.0f;
    };
    if (!ignored(left, in.leftRPM) && !ignored(right, in.rightRPM))
    {
        lowSpeedIgnored_ = false;
        return;
    }
    if (!lowSpeedIgnored_)
    {
        lowSpeedIgnored_ = true;
        lowSpeedSince_ = in.now;
    }
    if (!lowSpeedReported_ && in.now - lowSpeedSince_ >= 1500)
    {
        lowSpeedReported_ = true;
        telemetry::Telemetry::logf("Hint: a wheel is not following a low speed command (under %.0f RPM). If a small lean does "
                                   "nothing and then the chair lurches, lower s_pid_min_erpm in VESC Tool (default 900).\n",
                                   STOP_HANDOFF_RPM);
    }
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
    {
        // A tare with someone (or something) on the seat would hide that load
        // from then on: the rider would read as not seated, or the seat would
        // read empty in the middle of a ride.
        if (lean_.isSeated())
            telemetry::Telemetry::logf("Tare refused: the seat reads as occupied (load %d, threshold %d). Empty the seat. "
                                       "If it is empty, set the sensor zeros from the dashboard.\n",
                                       lean_.totalLoad(), settings::seatThreshold.get());
        else
            lean_.beginTare(in.now);
    }

    // After a watchdog restart the chair may be rolling with nobody in
    // control. Brake from the measured speed as soon as the VESCs report, or
    // blind if they stay silent for half a second.
    if (recoverFromReset_)
    {
        if (!recoverTimed_)
        {
            recoverTimed_ = true;
            recoverStartedAt_ = in.now;
        }
        if (fresh)
        {
            if (std::max(std::fabs(in.leftRPM), std::fabs(in.rightRPM)) >= STOP_DONE_RPM)
                stop_.begin(in.leftRPM, in.rightRPM, in.now);
            recoverFromReset_ = false;
        }
        else if (in.now - recoverStartedAt_ >= 500)
        {
            stop_.beginBlind(in.now);
            recoverFromReset_ = false;
        }
    }

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
        // Which way the chair is going, as a share of the speed limit: the
        // command when driving, else the wheels.
        const float maxRPM = speedLimitMS_ * LINEAR_TO_RPM;
        const float travelRPM = driving_ ? motion_.linearRPM() : (in.leftRPM + in.rightRPM) / 2.0f;
        const auto command = lean_.command(maxRPM > 0.0f ? travelRPM / maxRPM : 0.0f);
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
        if (lean_.tareActive())
        {
            if (lean_.isSeated())
            {
                lean_.cancelCalibration();
                telemetry::Telemetry::logf("Tare cancelled: the seat became occupied.\n");
            }
            else if (lean_.updateTare(in.now, in.calButtonHeld) == LeanDriveMode::Event::TareDone)
                saveCalibration();
        }
        return stopOrRelease(in);
    }
    }
}
