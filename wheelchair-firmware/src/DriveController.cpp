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
    case Fault::RemoteStop: return "REMOTE_STOP";
    default: return "NONE";
    }
}

void DriveController::begin(bool afterWatchdogReset)
{
    modeState_.begin(DISARM);
    bootRecovery_ = true;
    bootTimed_ = false;
    restingS_ = 0.0f;
    lean_.reset();
    remote_.reset();
    assist_.reset();
    motion_.reset();
    stop_.cancel();
    driving_ = false;
    freewheel_ = false;
    faultHold_ = afterWatchdogReset; // nobody was expecting that stop either
    reverseLocked_ = false;
    fault_ = afterWatchdogReset ? Fault::WatchdogReset : Fault::None;
}

void DriveController::applySettings()
{
    lean_.updateResponseCurve();
    // 0 means the VESCs regulate speed all the way down to a standstill: then
    // there is no zone to work around, and a zero speed command holds the chair.
    const float vescMin = settings::vescMinSpeed.get();
    STOP_HANDOFF_RPM = vescMin > 0.0f ? vescMin + STOP_HANDOFF_MARGIN_RPM : 0.0f;
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
    if (actions.disarmPressed)
        faultHold_ = false; // acknowledged: back to the normal DISARM rules
    if (actions.toggleFreewheel)
    {
        freewheel_ = !freewheel_;
        telemetry::Telemetry::logf(freewheel_ ? "Free-wheel: the empty chair can be pushed by hand. Press DISARM again to hold it.\n"
                                              : "Holding the empty chair again.\n");
    }
    return actions;
}

void DriveController::raiseFault(Fault fault)
{
    fault_ = fault;
    // In Assist the rider is propelling by hand: a fault only takes the boost
    // away. Everywhere else the chair is stopped and then held.
    // (The remote's stop button is a person asking for a stop: that brakes.)
    faultInAssist_ = modeState_.current() == ASSIST && fault != Fault::RemoteStop;
    faultHold_ = !faultInAssist_;
    // An unplugged sensor does not show up here: the sensors are two-wire,
    // so an open circuit reads the same as an unloaded sensor. (LeanDriveMode
    // makes sure that cannot create motion.)
    telemetry::Telemetry::logf(fault == Fault::SensorRail
                                   ? "Fault: a seat sensor is reading at an ADC rail (shorted wiring or a failed amplifier). Disarming.\n"
                                   : fault == Fault::RemoteStop
                                         ? "Stopped from the remote's stop button. Disarming; clear it on the remote before arming again.\n"
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
        const float from = sent >= 0.0f ? std::min(sent, std::max(wheel, 0.0f)) : std::max(sent, std::min(wheel, 0.0f));
        // The command was already near zero but this wheel is still fast (it
        // lags a braking command): start at the handoff speed, so the speed
        // loop goes on braking it down instead of handing over to brake
        // current at once.
        if (std::fabs(wheel) > STOP_HANDOFF_RPM + 4.0f && std::fabs(from) < STOP_HANDOFF_RPM)
            return std::copysign(STOP_HANDOFF_RPM, wheel);
        return from;
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
    freewheel_ = false;
    reverseLocked_ = false;
    if (modeState_.current() == DISARM)
    {
        if (previous == LEAN || previous == REMOTE)
        {
            if (driving_)
                beginStop(in, /*fromMeasured=*/false);
        }
        else if (previous == ASSIST && !faultInAssist_)
        {
            // DISARM stops the chair from every mode, and so does an empty
            // seat. Assist drives by current, so the stop starts from the
            // measured wheel speeds. (A fault in Assist only drops the boost.)
            beginStop(in, /*fromMeasured=*/true);
        }
        faultInAssist_ = false;
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
    {
        // A chair that comes under hold while it is rolling fast (LEAN or
        // REMOTE armed on a moving chair) is stopped the usual way, with the
        // ramp and its soft start, not grabbed with full brake current.
        const bool rolling = in.leftFresh && in.rightFresh &&
                             std::max(std::fabs(in.leftRPM), std::fabs(in.rightRPM)) >= std::max(STOP_HANDOFF_RPM, STOP_DONE_RPM);
        if (!(holds() && rolling))
            return parked(in);
        stop_.begin(in.leftRPM, in.rightRPM, in.now);
    }

    const bool fresh = in.leftFresh && in.rightFresh;
    const auto out = stop_.update(in.dt, in.now, {in.leftRPM, in.rightRPM, fresh,
                                                  std::max(std::fabs(in.leftAmps), std::fabs(in.rightAmps))});
    if (stop_.timedOutMoving())
        faultHold_ = true; // still rolling: keep it held until DISARM is pressed or a mode is armed
    if (stop_.rampIneffective() && !brakingHintGiven_)
    {
        brakingHintGiven_ = true;
        telemetry::Telemetry::logf("Hint: the wheels did not slow under the speed commands of this stop, so it went to brake "
                                   "current. Check that \"Allow Braking\" is on in the VESCs' speed PID settings.\n");
    }
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
        return {{MotorCommand::Kind::RPM, left}, {MotorCommand::Kind::RPM, right}};
    }
    case StopController::Drive::Brake:
        return {{MotorCommand::Kind::Brake, out.left}, {MotorCommand::Kind::Brake, out.right}};
    default:
        return parked(in); // the stop is over: held or released, by the usual rules
    }
}

// What the motors do when nothing is driving and no stop is running.
//
// A chair that is not being driven must not roll away. It used to be released
// here in every case, so after a stop on a ramp it rolled off, with or without
// its rider. Now it is held: any wheel that starts to turn gets brake current
// (which can slow a wheel but never drive it). The exceptions are the states
// in which someone is meant to move the chair by hand:
//   - Assist: the rider is pushing the rims.
//   - DISARM with a rider in the seat: it is a manual chair, and the rider
//     has the rims. (Not straight after a fault: the rider was not expecting
//     the stop, so the chair stays held until DISARM is pressed.)
//   - DISARM with the seat empty, after DISARM has been tapped to ask for
//     free-wheel (to push the empty chair around).
// Holding is damping, not a parking brake: on a slope the chair creeps.
bool DriveController::holds() const
{
    const common::mode mode = modeState_.current();
    return mode == LEAN || mode == REMOTE ||
           (mode == DISARM && (faultHold_ || (!lean_.isSeated() && !freewheel_)));
}

DriveController::MotorCommand DriveController::parked(const TickInput &in) const
{
    if (!holds())
        return release();
    if (!(in.leftFresh && in.rightFresh))
    {
        // No wheel speed to go by (a silent VESC): brake both and keep braking.
        return {{MotorCommand::Kind::Brake, STOP_BRAKE_AMPS}, {MotorCommand::Kind::Brake, STOP_BRAKE_AMPS}};
    }
    const auto wheel = [](float rpm) -> MotorCommand::Wheel {
        if (std::fabs(rpm) < DEADZONE_REST_RPM)
            return {MotorCommand::Kind::Current, 0.0f};
        return {MotorCommand::Kind::Brake,
                clampTo(DEADZONE_BRAKE_AMPS_PER_RPM * std::fabs(rpm), DEADZONE_BRAKE_MIN_AMPS, STOP_BRAKE_AMPS)};
    };
    return {wheel(in.leftRPM), wheel(in.rightRPM)};
}

// The largest turn (half the difference between the wheels) allowed right
// now: the full-lean turn rate, less at speed so that speed x turn rate
// (sideways acceleration) stays under its limit. The speed used is the faster
// of what is commanded and what the wheels report.
float DriveController::turnLimitRPM(const TickInput &in) const
{
    const float speedMS = std::max(std::fabs(motion_.linearRPM()),
                                   std::fabs(in.leftRPM + in.rightRPM) / 2.0f) / LINEAR_TO_RPM;
    float turnRate = settings::maxTurnRate.get() * DegToRad;
    const float lateral = settings::maxLateralAccel.get();
    if (speedMS * turnRate > lateral)
        turnRate = lateral / speedMS;
    return turnRate * settings::trackWidth.get() / 2.0f * LINEAR_TO_RPM;
}

DriveController::MotorCommand DriveController::driveRPM(const TickInput &in, float linearTargetRPM, float turnTargetRPM)
{
    const bool fresh = in.leftFresh && in.rightFresh;
    if (stop_.active())
    {
        // Drive came back mid-stop (e.g. a short remote dropout): carry on
        // from where the stop had got to instead of from zero.
        if (fresh) // see below: a chair above the speed-knob limit is not cut to it in one step
            speedLimitMS_ = std::max(speedLimitMS_, std::min(MAX_SPEED, std::fabs(in.leftRPM + in.rightRPM) / 2.0f / LINEAR_TO_RPM));
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
        // so the command does not restart at zero and brake hard. If that is
        // above the speed-knob limit, the limit starts there too and comes
        // down at the design rate, instead of cutting the command in one step.
        if (fresh)
        {
            speedLimitMS_ = std::max(speedLimitMS_, std::min(MAX_SPEED, std::fabs(in.leftRPM + in.rightRPM) / 2.0f / LINEAR_TO_RPM));
            motion_.seed(in.leftRPM, in.rightRPM);
        }
        else
        {
            motion_.reset();
        }
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
        // One wheel's bad speed reading must not lift the other wheel's
        // command: neither goes past the speed limit, unless the wheel itself
        // is already faster than that.
        const float maxRPM = speedLimitMS_ * LINEAR_TO_RPM;
        const float leftCap = std::max(maxRPM, std::fabs(in.leftRPM)), rightCap = std::max(maxRPM, std::fabs(in.rightRPM));
        left = clampTo(left, -leftCap, leftCap);
        right = clampTo(right, -rightCap, rightCap);
        // The command itself may not run further ahead than what is sent.
        motion_.limitLead(measuredLinear, measuredTurn, MAX_RPM_COMMAND_DIFF);
    }
    commands_ = {linearTargetRPM + turnTargetRPM, linearTargetRPM - turnTargetRPM, left, right};
    sentLeftRPM_ = left;
    sentRightRPM_ = right;
    if (!fresh)
        return {{MotorCommand::Kind::RPM, left}, {MotorCommand::Kind::RPM, right}};
    watchLowSpeed(in, left, right);

    // A VESC's speed loop does not run when the command is under its minimum
    // ERPM (older VESC firmware then releases the motor; see
    // drive_constants.h). So a wheel still turning faster than such a command
    // would coast: after a turn the chair kept rotating, after a run it rolled
    // on, and on a slope it rolled away at neutral. That wheel gets brake
    // current instead, in proportion to how much too fast it is.
    const auto wheelCommand = [](float command, float wheel) -> MotorCommand::Wheel {
        if (std::fabs(command) >= STOP_HANDOFF_RPM)
            return {MotorCommand::Kind::RPM, command};
        // Wheel still well above the zone (it lags a braking command): hold
        // the command at the edge, so the speed loop keeps braking it down.
        if (std::fabs(wheel) > STOP_HANDOFF_RPM + 4.0f)
            return {MotorCommand::Kind::RPM, std::copysign(STOP_HANDOFF_RPM, wheel)};
        if (std::fabs(wheel) >= DEADZONE_REST_RPM)
        {
            // Too fast by: all of its speed if it turns the other way.
            const float excess = command * wheel < 0.0f ? std::fabs(wheel) : std::fabs(wheel) - std::fabs(command);
            if (excess > 0.0f)
                return {MotorCommand::Kind::Brake,
                        clampTo(DEADZONE_BRAKE_AMPS_PER_RPM * excess, DEADZONE_BRAKE_MIN_AMPS, STOP_BRAKE_AMPS)};
        }
        return {MotorCommand::Kind::RPM, command};
    };
    return {wheelCommand(left, in.leftRPM), wheelCommand(right, in.rightRPM)};
}

// Diagnostic only. A VESC in speed mode does not drive below its minimum
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
    if (lean_.isSeated())
        freewheel_ = false; // someone sat down: back to the normal rules
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
        if ((selected == LEAN || selected == ASSIST) && remote_.softStopLatched())
            // The remote's stop button works in every mode: whoever holds the
            // remote can stop a rider in LEAN or Assist. (In REMOTE it brakes
            // the chair and waits there, as before.)
            raiseFault(Fault::RemoteStop);
        else if ((selected == LEAN || selected == ASSIST) && lean_.sensorAtRail())
            raiseFault(Fault::SensorRail); // the seat reading cannot be trusted either
        else if (!fresh)
            raiseFault(Fault::VescSilent); // every drive mode needs wheel-speed feedback
        else
        {
            fault_ = Fault::None;
            faultHold_ = false;
        }
    }

    // ---- Calibration button -------------------------------------------------
    bool calibrationBegan = false;
    if (calibrationRequested && modeState_.current() == LEAN && lean_.isSeated() && !lean_.calibrationActive())
    {
        // A button bumped at speed must not stop the chair. A chair that is
        // only creeping (the reason to recalibrate) is stopped and calibrated.
        if (fresh && std::max(std::fabs(in.leftRPM), std::fabs(in.rightRPM)) > CALIBRATE_MAX_RPM)
            telemetry::Telemetry::logf("Calibration ignored: the chair is moving. Slow down first.\n");
        else
        {
            lean_.beginCalibration(in.now);
            calibrationBegan = true;
        }
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

    // At power-on the chair may be rolling with nobody in control (a reset or
    // a brown-out while driving). Brake from the measured speed as soon as the
    // VESCs report, or blind if they stay silent for half a second. A chair
    // that is standing still is left alone.
    if (bootRecovery_)
    {
        if (!bootTimed_)
        {
            bootTimed_ = true;
            bootStartedAt_ = in.now;
        }
        if (fresh)
        {
            if (std::max(std::fabs(in.leftRPM), std::fabs(in.rightRPM)) >= STOP_DONE_RPM)
                stop_.begin(in.leftRPM, in.rightRPM, in.now);
            bootRecovery_ = false;
        }
        else if (in.now - bootStartedAt_ >= 500)
        {
            stop_.beginBlind(in.now);
            bootRecovery_ = false;
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
        // Leaning back is how the rider brakes, and a startled rider leans
        // back and stays there. So a back lean that began while the chair was
        // rolling forward only brakes it to a standstill. Reverse needs the
        // rider to come back to neutral first, and is slower than forward.
        if (travelRPM > STOP_DONE_RPM)
            reverseLocked_ = true;
        const auto command = lean_.command(maxRPM > 0.0f ? travelRPM / maxRPM : 0.0f, reverseLocked_);
        if (command.stopped)
        {
            restingS_ = 0.0f;
            return stopOrRelease(in);
        }
        if (travelRPM <= STOP_DONE_RPM && command.speed == 0.0f)
            reverseLocked_ = false;

        // Standing still at neutral for a while: let neutral follow the
        // rider's posture and the sensors' slow drift (LeanDriveMode::trimNeutral).
        const bool resting = command.speed == 0.0f && command.turn == 0.0f && fresh &&
                             std::max(std::fabs(in.leftRPM), std::fabs(in.rightRPM)) < STOP_DONE_RPM;
        restingS_ = resting ? restingS_ + in.dt : 0.0f;
        if (restingS_ >= NEUTRAL_TRIM_AFTER_S)
            lean_.trimNeutral(in.dt);

        float speed = command.speed;
        if (speed < 0.0f)
            speed = reverseLocked_ ? 0.0f : speed * REVERSE_SPEED_FRACTION;

        // Turn is a rate in deg/s, not a share of the speed knob.
        const float turnTargetRPM = command.turn * turnLimitRPM(in);
        return driveRPM(in, speed * maxRPM, turnTargetRPM);
    }

    case REMOTE:
    {
        const auto targets = remote_.getTargets(in.now);
        if (remote_.stopped())
            return stopOrRelease(in);
        const float maxRPM = speedLimitMS_ * LINEAR_TO_RPM;
        const float left = clampTo(targets.left, -1.0f, 1.0f) * maxRPM;
        const float right = clampTo(targets.right, -1.0f, 1.0f) * maxRPM;
        // The same limits as LEAN: reverse is slower, and the turn is held to
        // the turn-rate and sideways-acceleration limits (stick hard over at
        // full speed asked for a pivot far past what the chair can take).
        float linear = (left + right) / 2.0f;
        if (linear < 0.0f)
            linear *= REVERSE_SPEED_FRACTION;
        const float turnLimit = turnLimitRPM(in);
        return driveRPM(in, linear, clampTo((left - right) / 2.0f, -turnLimit, turnLimit));
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
        return {{MotorCommand::Kind::Current, assistCurrent_.left}, {MotorCommand::Kind::Current, assistCurrent_.right}};
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
