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
    enum class Fault : uint8_t { None, SensorRail, VescSilent, WatchdogReset, RemoteStop };
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
    // A profile was loaded: start again from rest and require a new calibration.
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
    const telemetry::MotorCommands &commands() const { return commands_; }
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
    void saveCalibration();
    void watchLowSpeed(const TickInput &input, float left, float right);

    ModeStateMachine modeState_;
    LeanDriveMode lean_;
    RemoteDriveMode remote_;
    AssistDriveMode assist_;
    MotionSmoother motion_{RPMSetpointPID::defaultTuning()};
    StopController stop_;

    Fault fault_ = Fault::None;
    bool calPressed_ = false; // calibration button pressed since the last tick
    // True while wheel-speed commands from the smoother are being sent, so a
    // loss of drive knows there is motion to stop.
    bool driving_ = false;
    // Set at power-on until the boot recovery stop has been decided.
    bool bootRecovery_ = false;
    uint32_t bootStartedAt_ = 0;
    bool bootTimed_ = false;
    // How long the chair has stood still at neutral lean (for the neutral trim).
    float restingS_ = 0.0f;
    // DISARM was tapped with the seat empty: do not hold the chair.
    bool freewheel_ = false;
    // A fault stopped the chair: hold it, rider aboard or not, until someone
    // presses DISARM or arms a mode.
    bool faultHold_ = false;
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
