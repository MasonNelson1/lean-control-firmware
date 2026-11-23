#pragma once

#include <cstddef>
#include <cstdint>

#include "ModeStateMachine.h"
#include "driveModes/AssistDriveMode.h"
#include "driveModes/LeanDriveMode.h"
#include "driveModes/RemoteDriveMode.h"
#include "motion/MotionSmoother.h"
#include "motion/StopController.h"
#include "telemetry/TelemetryTypes.h"

// Everything between the inputs (buttons, seat sensors, remote packets, VESC
// feedback) and the one motor command sent each control tick: the mode state
// machine, fault checks, the drive modes, motion smoothing and stopping.
//
// It touches hardware only through analogRead (the seat sensors) and the
// settings registry, so tests/native/drive_controller_test.cpp can run whole
// rides on the host. main.cpp does the rest: buttons, CAN, LEDs, serial.
class DriveController
{
public:
    // CopJump (council-corner): corner layout, the centre of pressure moved
    // further than lean::MaxCopStep in one control tick (a sensor dropping out).
    enum class Fault : uint8_t { None, SensorRail, VescSilent, WatchdogReset, RemoteStop, CopJump };
    static const char *faultName(Fault fault);

    // What to send to the VESCs this tick. Each wheel has its own kind of
    // command: in normal driving one wheel can be braking while the other is
    // under speed control.
    struct MotorCommand
    {
        enum class Kind : uint8_t { Current, RPM, Brake };
        struct Wheel
        {
            Kind kind;
            float value; // amps, wheel RPM, or brake amps
        };
        Wheel left, right;
    };

    struct TickInput
    {
        uint32_t now = 0;  // milliseconds
        float dt = 0.005f; // seconds since the previous tick
        // Measured wheel speeds, and whether each VESC reported recently.
        float leftRPM = 0, rightRPM = 0;
        bool leftFresh = false, rightFresh = false;
        // Motor current each VESC reports (A; negative when braking).
        float leftAmps = 0, rightAmps = 0;
        float speedLimitMS = 0;     // speed-knob selection
        bool calButtonHeld = false; // raw level of the calibration button
    };

    // The chair always starts in DISARM. It may be rolling (a reset or a
    // brown-out while driving), so once the VESCs report it brakes to a stop
    // if the wheels are turning, or brakes blind if they stay silent.
    // afterWatchdogReset: the firmware hung and was restarted; shown as a fault.
    void begin(bool afterWatchdogReset = false);
    // Derived settings changed (dashboard edit, profile load, startup).
    void applySettings();
    // A profile was loaded: start again from rest and require a new calibration
    // (and drop a hold-calibrated reach, as dev-setup did).
    void resetForProfile();

    // Every loop: button events. Fills in the seat and calibration inputs.
    ModeStateMachine::Actions updateButtons(ModeStateMachine::Inputs input, uint32_t now);
    void onRemotePacket(const uint8_t *buffer, size_t size, uint32_t now) { remote_.onPacketReceived(buffer, size, now); }
    // Every control period.
    MotorCommand tick(const TickInput &input);

    common::mode mode() const { return modeState_.current(); }
    const ModeStateMachine &modeState() const { return modeState_; }
    const LeanDriveMode &lean() const { return lean_; }
    bool remoteStopped() const { return remote_.stopped(); }
    // The fault that caused the last disarm; cleared when a mode is armed again.
    Fault fault() const { return fault_; }
    bool stopping() const { return stop_.active(); }
    // The empty chair has been let go to be pushed by hand (see parked()).
    bool freewheeling() const { return freewheel_; }
    // DISARM after an automatic disarm (fault, empty seat) and the chair is
    // still braking or being held. Cleared by a DISARM press or arming a mode.
    // main.cpp blinks the DISARM LED on it.
    bool autoBraking() const;
    // True once after a hold calibration wrote new reach settings (main.cpp
    // saves them to the active profile).
    bool takeReachSave() { const bool p = reachSave_; reachSave_ = false; return p; }
    const telemetry::MotorCommands &commands() const { return commands_; }
    const MotionSmoother &motion() const { return motion_; }
    float speedLimitMS() const { return speedLimitMS_; } // slewed speed cap, for DBG M
    // Corner layout: call on every 20 Hz telemetry tick. Re-zeroes the
    // empty-plate baseline (RAM only) while DISARMED with the seat empty and
    // no stop running; also forgets a hold-calibrated reach after 3 s empty.
    void trackEmptyPlate(uint32_t now) { lean_.trackEmptyPlate(now, modeState_.current() == common::DISARM && !stop_.active()); }
    MotorTargets assistCurrent() const { return assistCurrent_; }

private:
    void raiseFault(Fault fault);
    void applySeat(const TickInput &input);
    void beginStop(const TickInput &input, bool fromMeasured);
    MotorCommand stopOrRelease(const TickInput &input);
    MotorCommand driveRPM(const TickInput &input, float linearTargetRPM, float turnTargetRPM);
    static MotorCommand release() { return {{MotorCommand::Kind::Current, 0.0f}, {MotorCommand::Kind::Current, 0.0f}}; }
    bool holds() const;
    MotorCommand parked(const TickInput &input) const;
    float turnLimitRPM(const TickInput &input) const;
    void watchLowSpeed(const TickInput &input, float left, float right);

    ModeStateMachine modeState_;
    LeanDriveMode lean_;
    RemoteDriveMode remote_;
    AssistDriveMode assist_;
    MotionSmoother motion_{RPMSetpointPID::defaultTuning()};
    StopController stop_;

    Fault fault_ = Fault::None;
    uint32_t now_ = 0; // time of the current tick, for log lines
    // True while wheel-speed commands from the smoother are being sent, so a
    // loss of drive knows there is motion to stop.
    bool driving_ = false;
    // Set at power-on until the boot recovery stop has been decided.
    bool bootRecovery_ = false;
    uint32_t bootStartedAt_ = 0;
    bool bootTimed_ = false;
    // DISARM was tapped with the seat empty: do not hold the chair.
    bool freewheel_ = false;
    // A fault stopped the chair: hold it, rider aboard or not, until someone
    // presses DISARM or arms a mode.
    bool faultHold_ = false;
    bool autoBrake_ = false; // the last disarm was automatic (see autoBraking())
    bool reachSave_ = false;
    bool faultInAssist_ = false; // the fault being handled this tick was raised in Assist
    // A back lean that began while rolling forward only brakes (see tick()).
    bool reverseLocked_ = false;
    float sentLeftRPM_ = 0.0f, sentRightRPM_ = 0.0f;
    float speedLimitMS_ = 0.0f;
    bool brakingHintGiven_ = false;
    // Low-speed diagnostic (watchLowSpeed).
    bool lowSpeedIgnored_ = false, lowSpeedReported_ = false;
    uint32_t lowSpeedSince_ = 0;
    telemetry::MotorCommands commands_{};
    MotorTargets assistCurrent_{};
};
