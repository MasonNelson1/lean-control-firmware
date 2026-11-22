#include <Arduino.h>
#include <ezButton.h>
#include "vesc_can.h"
#include "rpm_setpoint_pid.h"
#include "common.h"
#include "driveModes/DriveMode.h"
#include "driveModes/LeanDriveMode.h"
#include "driveModes/RemoteDriveMode.h"
#include "driveModes/DisarmDriveMode.h"
#include "driveModes/AssistDriveMode.h"
#include "PacketSerial.h"
#include "config/DriveSettings.h"
#include "drive_knobs.h"
#include "ModeStateMachine.h"
#include "telemetry/Telemetry.h"

using namespace common;

// Runtime knob selections, set by updateKnobs(). Start at the gentlest
// position until the knobs are read in setup().
knobs::RotarySwitch speedKnob;
knobs::RotarySwitch profileKnob;
static int lastProfilePosition = knobs::Invalid;
static int pendingProfile = knobs::Invalid;
static int activeProfile = knobs::Invalid;
float userMaxSpeedMS = knobs::speedLimitMS(0);
// TODO: a stop/fault should bypass smoothing and reset BOTH PID instances.

// MISC CONSTANTS and vars
ezButton calibrateButton(PIN_CAL_BUTTON);
ezButton leanModeButton(PIN_LEAN_BUTTON);
ezButton remoteModeButton(PIN_REMOTE_BUTTON);
ezButton disarmModeButton(PIN_DISARM_BUTTON);
static uint32_t calibrationPressedAt = 0;

RPMSetpointPID leftRPMSetpointPID(RPMSetpointPID::defaultTuning());
RPMSetpointPID rightRPMSetpointPID(RPMSetpointPID::defaultTuning());

VescBus can1;

// Cached motor addressing.
struct MotorSetup
{
    uint8_t leftId;
    uint8_t rightId;
};

static MotorSetup motors;
static VescStatus measuredMotors[2]{};

static telemetry::MotorCommands motorTelemetry{};
static telemetry::Telemetry telemetryOutput;
static MotorTargets assistBoostCurrent{};

// This function takes doubles from -1 to 1 for the left and right motors and sets their speeds to those values
// where the value is the percentage of the speed-knob max speed (full input = selected top speed).
// PID smoothing is applied prior to using the setpoints.
void setMotorPower(double left, double right, float dt)
{
    // Turning the speed knob up takes effect at once (the PID still limits
    // acceleration). Turning it down lowers the cap no faster than the design
    // braking rate, so the smoother never sees a step drop in its speed limit.
    static float speedLimitMS = 0.0f;
    speedLimitMS = knobs::slewDown(speedLimitMS, userMaxSpeedMS, MAX_ACCEL, dt);

    const float leftTargetRPM = constrain(left, -1.0f, 1.0f) * speedLimitMS * LINEAR_TO_RPM;
    const float rightTargetRPM = constrain(right, -1.0f, 1.0f) * speedLimitMS * LINEAR_TO_RPM;
    const float leftMeasuredRPM = measuredMotors[0].erpm / VESC_ERPM_PER_WHEEL_RPM;
    const float rightMeasuredRPM = measuredMotors[1].erpm / VESC_ERPM_PER_WHEEL_RPM;
    const float leftRpms = constrain(
        leftRPMSetpointPID.smoothRPM(leftTargetRPM, dt, speedLimitMS, MAX_ACCEL),
        leftMeasuredRPM - common::MAX_RPM_COMMAND_DIFF, leftMeasuredRPM + common::MAX_RPM_COMMAND_DIFF);
    const float rightRpms = constrain(
        rightRPMSetpointPID.smoothRPM(rightTargetRPM, dt, speedLimitMS, MAX_ACCEL),
        rightMeasuredRPM - common::MAX_RPM_COMMAND_DIFF, rightMeasuredRPM + common::MAX_RPM_COMMAND_DIFF);
    vescSetWheelRPM(can1, motors.leftId, leftRpms);
    vescSetWheelRPM(can1, motors.rightId, rightRpms);
    motorTelemetry = {leftTargetRPM, rightTargetRPM, leftRpms, rightRpms};
}

ModeStateMachine modeState;
LeanDriveMode leanMode;
AssistDriveMode assistMode;

PacketSerial packetSerial;
RemoteDriveMode remoteMode;
DisarmDriveMode disarmMode;

static void onBluetoothPacket(const uint8_t *buffer, size_t size)
{
    // PacketSerial calls this with decoded bytes. Configuration packets enter
    // their own request slot, and ordinary remote packets keep their handler.
    if (!settings::protocol.queue(buffer, size))
        remoteMode.onPacketReceived(buffer, size);
}

static void applyRuntimeSettings()
{
    // Refresh values cached outside the registry after a successful mutation.
    // setTuning() preserves the existing PID motion and error history.
    LINEAR_TO_RPM = 60.0f / settings::wheelCircumference.get();
    motors = {settings::leftVesc.get(), settings::rightVesc.get()};
    calibrateButton.setDebounceTime(settings::buttonDebounce.get());
    leanModeButton.setDebounceTime(settings::buttonDebounce.get());
    remoteModeButton.setDebounceTime(settings::buttonDebounce.get());
    disarmModeButton.setDebounceTime(settings::buttonDebounce.get());
    leanMode.updateResponseCurve();
    leanMode.settingsChanged();
    const auto tuning = settings::normalPid.tuning();
    leftRPMSetpointPID.setTuning(tuning);
    rightRPMSetpointPID.setTuning(tuning);
}

// Interrupts latch raw presses; ezButton confirms the debounced press in loop().
// Never call ezButton, print, or command motors from an interrupt handler.
static volatile bool leanEdgePending = false;
static volatile bool remoteEdgePending = false;
static volatile bool disarmEdgePending = false;

void onLeanButtonPress()
{
    leanEdgePending = true;
}

void onRemoteButtonPress()
{
    remoteEdgePending = true;
}

void onDisarmButtonPress()
{
    disarmEdgePending = true;
}
// Apply the knobs' current positions and report them as one text line.
// Non-CSV lines show up as console text in the debug viewer.
static void applyKnobs()
{
    userMaxSpeedMS = knobs::speedLimitMS(speedKnob.position());

    // this only prints when the knobs are moved, not all the time.
    telemetry::Telemetry::logf(
        "Knobs: speed %d/%d (%.2f m/s, raw %u)%s, profile %d/%d (raw %u)%s\n",
        speedKnob.position() + 1, knobs::Positions, userMaxSpeedMS,
        static_cast<unsigned>(speedKnob.lastRaw()), speedKnob.faulted() ? " FAULT->slowest" : "",
        profileKnob.position() + 1, knobs::Positions,
        static_cast<unsigned>(profileKnob.lastRaw()), profileKnob.faulted() ? " FAULT" : "");
}

static bool activateProfile(int slot)
{
    if (!settings::protocol.selectProfile(slot))
        return false;
    activeProfile = slot;
    applyRuntimeSettings();
    leanMode.reset();
    leanMode.resetDynamicGains();
    assistMode.reset();
    leftRPMSetpointPID.reset();
    rightRPMSetpointPID.reset();
    motorTelemetry = {};
    for (auto &motor : measuredMotors)
        motor = {};
    return true;
}

static void updateProfileLED()
{
    constexpr uint32_t pulseMs = 200;
    constexpr uint32_t gapMs = 2000;
    static bool wasSelecting = false;
    static int displayedProfile = knobs::Invalid;
    static uint32_t cycleStart = 0;
    const uint32_t now = millis();
    const bool selecting = pendingProfile != knobs::Invalid;
    if (displayedProfile != activeProfile || (wasSelecting && !selecting))
    {
        displayedProfile = activeProfile;
        cycleStart = now;
    }
    wasSelecting = selecting;

    bool ledOn = selecting;
    if (!selecting && activeProfile != knobs::Invalid)
    {
        const uint32_t burstMs = (2 * (activeProfile + 1) - 1) * pulseMs;
        const uint32_t phase = (now - cycleStart) % (burstMs + gapMs);
        cycleStart = now - phase;
        ledOn = phase < burstMs && phase % (2 * pulseMs) < pulseMs;
    }
    digitalWrite(PIN_CONNECTED_LED, ledOn ? HIGH : LOW);
}

static void updateKnobs()
{
    static uint32_t lastSampleMs = 0;
    const uint32_t now = millis();
    if (now - lastSampleMs < knobs::SampleMs)
        return;
    lastSampleMs = now;

    const bool speedChanged = speedKnob.update(analogRead(PIN_SPEED), now);
    const bool profileChanged = profileKnob.update(analogRead(PIN_ACCEL), now);
    if (profileKnob.settled() && profileKnob.position() != lastProfilePosition)
    {
        lastProfilePosition = profileKnob.position();
        pendingProfile = lastProfilePosition;
    }
    if (speedChanged || profileChanged)
        applyKnobs();
}
void setup()
{
    // Worst-case burst when the 1 Hz lines and a 20 Hz group land in one loop:
    // DYNAMIC_GAINS ≈45, DBG SENSORS ≈150, DBG CORNER ≈150, 2× DBG VESC ≈170,
    // DBG R ≈30, DBG COP ≈40, CSV row ≈110: ≈700 bytes, plus room for the VESC
    // probe lines on charles/dev-setup (≈100). Telemetry drops whole lines rather
    // than block when this is full.
    static uint8_t bluetoothTxBuffer[1024];

    // set up serial and telemetry stuff
    Serial.begin(115200);
    Serial1.begin(115200);
    Serial1.addMemoryForWrite(bluetoothTxBuffer, sizeof(bluetoothTxBuffer));
    packetSerial.setStream(&Serial1);
    packetSerial.setPacketHandler(onBluetoothPacket);

    // setup led's
    pinMode(common::PIN_CAL_LED, OUTPUT);
    pinMode(common::PIN_REMOTE_LED, OUTPUT);
    pinMode(common::PIN_LEAN_LED, OUTPUT);
    pinMode(common::PIN_DISARM_LED, OUTPUT);
    pinMode(common::PIN_CONNECTED_LED, OUTPUT);

    settings::persistence.load();
    modeState.begin(settings::defaultMode.get());
    if (modeState.selected() == LEAN)
        leanMode.reset();
    applyRuntimeSettings();

    leanMode.resetDynamicGains();

    // user input
    analogReadResolution(12);
    analogReadAveraging(4);
    // Take the knobs' power-on positions before the first drive tick. An
    // invalid reading leaves that knob at its slowest position.
    speedKnob.seed(analogRead(PIN_SPEED), millis());
    const auto profileRaw = analogRead(PIN_ACCEL);
    profileKnob.seed(profileRaw, millis());
    lastProfilePosition = knobs::classify(profileRaw);
    if (lastProfilePosition != knobs::Invalid)
        activateProfile(lastProfilePosition);
    applyKnobs();
    // ezButton configures the pins as INPUT_PULLUP: pressing connects to GND.

    attachInterrupt(digitalPinToInterrupt(PIN_LEAN_BUTTON), onLeanButtonPress, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_REMOTE_BUTTON), onRemoteButtonPress, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_DISARM_BUTTON), onDisarmButtonPress, FALLING);

    // motors
    vescBegin(can1);
    delay(200);
    telemetry::Telemetry::logf("Mode: %s\n", modeName(modeState.current()));
    telemetry::Telemetry::logf("Setup successful, ready to drive.\n");
}

// Hardware adapter: the state machine owns all pending presses and timers.
static void updateModeInputs()
{
    ModeStateMachine::Inputs input;
    noInterrupts();
    input.lean.edge = leanEdgePending;
    input.remote.edge = remoteEdgePending;
    input.disarm.edge = disarmEdgePending;
    leanEdgePending = remoteEdgePending = disarmEdgePending = false;
    interrupts();

    leanModeButton.loop();
    remoteModeButton.loop();
    calibrateButton.loop();
    disarmModeButton.loop();
    input.lean.pressed = leanModeButton.isPressed();
    input.lean.released = leanModeButton.isReleased();
    input.remote.pressed = remoteModeButton.isPressed();
    input.remote.released = remoteModeButton.isReleased();
    input.disarm.pressed = disarmModeButton.isPressed();
    input.disarm.released = disarmModeButton.isReleased();
    input.calibrate.pressed = calibrateButton.isPressed();
    if (input.calibrate.pressed)
        calibrationPressedAt = millis();
    input.calibrate.released = calibrateButton.isReleased();
    input.leanHeld = leanModeButton.getStateRaw() == LOW;
    input.profileSettled = profileKnob.settled();
    input.pendingProfile = pendingProfile;
    input.calibrationActive = leanMode.calibrationActive();
    // Refresh only the seat reading, leaving the control/telemetry sample intact.
    if (modeState.needsSeatSample(input))
        leanMode.sampleSensors(/*seatOnly=*/true);
    input.seated = leanMode.isSeated();
    const auto actions = modeState.update(input, millis());
    if (actions.resetRemote)
        remoteMode.reset();
    if (actions.resetAssist)
        assistMode.reset();
    if (actions.activateProfile != ModeStateMachine::NoProfile)
    {
        if (!activateProfile(actions.activateProfile))
            telemetry::Telemetry::logf("Profile %d unavailable; keeping current settings.\n", actions.activateProfile + 1);
        pendingProfile = knobs::Invalid;
    }
}

static void updateModeLEDs()
{
    switch (modeState.current())
    {
    case LEAN:
        digitalWrite(common::PIN_LEAN_LED, HIGH);
        digitalWrite(common::PIN_DISARM_LED, LOW);
        digitalWrite(common::PIN_REMOTE_LED, LOW);
        digitalWrite(common::PIN_CAL_LED, leanMode.calibrationLEDOn(millis()) ? HIGH : LOW);
        break;

    case REMOTE:
        digitalWrite(common::PIN_LEAN_LED, LOW);
        digitalWrite(common::PIN_DISARM_LED, LOW);
        digitalWrite(common::PIN_REMOTE_LED, HIGH);
        digitalWrite(common::PIN_CAL_LED, LOW);
        break;
    case DISARM:
        digitalWrite(common::PIN_LEAN_LED, LOW);
        digitalWrite(common::PIN_DISARM_LED, HIGH);
        digitalWrite(common::PIN_REMOTE_LED, LOW);
        digitalWrite(common::PIN_CAL_LED, LOW);
        break;
    case ASSIST:
        digitalWrite(common::PIN_LEAN_LED, LOW);
        digitalWrite(common::PIN_DISARM_LED,
                     modeState.assistLEDOn(millis()) ? HIGH : LOW);
        digitalWrite(common::PIN_REMOTE_LED, LOW);
        digitalWrite(common::PIN_CAL_LED, LOW);
        break;
    default:
        break;
    }
}

static void applySeatedLatch()
{
    const auto transition = modeState.applySeat(leanMode.isSeated(), millis());
    if (!transition.changed)
        return;
    // Clear old motion on every entry to Disarm, including the mode button.
    if (modeState.current() == DISARM)
    {
        leanMode.cancelCalibration();
        leftRPMSetpointPID.reset();
        rightRPMSetpointPID.reset();
        assistMode.reset();
    }
    if (transition.resetLean)
        leanMode.reset();
    telemetry::Telemetry::logf("Mode: %s\n", modeName(modeState.current()));
}

static void updateDrive()
{
    // rate limit the loop.
    // this is placed right before the motor communication and pid smoothing because pid needs to run at a fixed rate.
    static uint32_t tSend = 0;
    const uint32_t now = millis();
    const uint32_t period = settings::controlPeriod.get();
    if (now - tSend < period)
    {
        return;
    }
    tSend = now;

    const bool calibrationRequested = modeState.takeCalibrationRequest();
    if (modeState.requiresSeat())
        leanMode.sampleSensors();
    // On (Charles, 2026-11-21): normal readings peak far below the rails (max ~1300 of 4095).
    // Rail hits are also reported in the DBG SENSORS line as railed=1 (issue #18).
    constexpr bool SENSOR_RAIL_DISARM = true;
    if (SENSOR_RAIL_DISARM && modeState.current() == LEAN && leanMode.sensorAtRail())
    {
        telemetry::Telemetry::logf("Sensor reading at ADC rail; disarming.\n");
        modeState.requestDisarm();
    }
    applySeatedLatch();
    if (modeState.current() == LEAN && leanMode.isSeated() && calibrationRequested)
    {
        vescSetCurrent(can1, motors.leftId, 0.0f);
        vescSetCurrent(can1, motors.rightId, 0.0f);
        leftRPMSetpointPID.reset();
        rightRPMSetpointPID.reset();
        motorTelemetry = {};
        leanMode.beginCalibration(calibrationPressedAt);
        leanMode.sampleSensors();
        // Calibration updates the seat threshold as well as the neutral floors.
        applySeatedLatch();
    }
    if (leanMode.calibrationActive())
    {
        if (leanMode.updateCalibration(millis(), calibrateButton.getState() == LOW))
        {
            modeState.requestDisarm();
            applySeatedLatch();
        }
        leanMode.sampleSensors();
        vescSetCurrent(can1, motors.leftId, 0.0f);
        vescSetCurrent(can1, motors.rightId, 0.0f);
        assistBoostCurrent = {};
        return;
    }

    const float dt = period / 1000.0f;
    MotorTargets targets;
    switch (modeState.current())
    {
    case LEAN:
        targets = leanMode.getTargetsFromSample();
        // Corner layout: a CoP step no body can make in one tick is a sensor
        // dropping out (lean::MaxCopStep). getTargetsFromSample already returned
        // zero; disarm like a rail fault.
        if (SENSOR_RAIL_DISARM && leanMode.centreOfPressureJumped())
        {
            telemetry::Telemetry::logf("Centre of pressure jumped; sensor fault suspected; disarming.\n");
            modeState.requestDisarm();
        }
        break;

    case REMOTE:
        targets = remoteMode.getTargets();
        break;
    case DISARM:
        targets = disarmMode.getTargets();
        break;
    case ASSIST:
        assistMode.loop(measuredMotors[0].erpm / VESC_ERPM_PER_WHEEL_RPM,
                        measuredMotors[1].erpm / VESC_ERPM_PER_WHEEL_RPM,
                        userMaxSpeedMS * LINEAR_TO_RPM);
        targets = assistMode.getTargets();
        break;
    default:
        targets = {0.0f, 0.0f};
        telemetry::Telemetry::logf("Unknown mode detected, setting motor power to 0, this should never happen\n");
        break;
    }
    assistBoostCurrent = modeState.current() == ASSIST ? targets : MotorTargets{};
    if (modeState.current() == LEAN || modeState.current() == REMOTE)
    {
        setMotorPower(targets.left, targets.right, dt);
    }
    else if (modeState.current() == DISARM)
    {
        vescSetCurrent(can1, motors.leftId, 0.0f);
        vescSetCurrent(can1, motors.rightId, 0.0f);
    }
    else
    {
        vescSetCurrent(can1, motors.leftId, targets.left);
        vescSetCurrent(can1, motors.rightId, targets.right);
    }
}

static void replyToConfiguration()
{
    constexpr size_t MaxPacketBytes = config::MaxResponseSize + config::MaxResponseSize / 254 + 3;
    settings::protocol.tick(millis(), modeState.current() == DISARM);
    if (!settings::protocol.pending() || Serial1.availableForWrite() < static_cast<int>(MaxPacketBytes))
        return;

    uint8_t response[config::MaxResponseSize];
    // Configuration writes are permitted only while the current mode is DISARM.
    // Only successful runtime mutations require refreshing derived settings.
    const uint32_t revision = settings::protocol.revision();
    const size_t length = settings::protocol.reply(response, modeState.current() == DISARM);
    if (settings::protocol.revision() != revision)
        applyRuntimeSettings();
    // The leading zero distinguishes PacketSerial responses from legacy CSV.
    Serial1.write(static_cast<uint8_t>(0));
    packetSerial.send(response, length);
}

static void publishTelemetry()
{
    const uint32_t now = millis();
    static uint32_t lastGainStatusMs = 0;
    if (now - lastGainStatusMs >= 1000)
    {
        lastGainStatusMs = now;
        leanMode.publishDynamicGains();
        leanMode.publishSensorStatus();
        leanMode.publishCornerStatus();
        // Motor controller health; age = ms since the last status frame (large = VESC silent).
        for (const VescStatus *v : {&measuredMotors[0], &measuredMotors[1]})
            telemetry::Telemetry::logf("DBG VESC id=%u age=%lu frames=%lu erpm=%ld current=%.1f duty=%.3f fetC=%.1f vin=%.1f\n",
                                       v == &measuredMotors[0] ? motors.leftId : motors.rightId,
                                       v->frames ? static_cast<unsigned long>(now - v->lastMs) : 0UL,
                                       static_cast<unsigned long>(v->frames), static_cast<long>(v->erpm),
                                       v->current, v->duty, v->mosfetTempC, v->inputVoltage);
    }
    if (!telemetryOutput.due(now))
        return;
    // Lean samples on each control tick; other modes sample for display only.
    if (modeState.current() != LEAN)
        leanMode.sampleSensors();
    // Corner layout: re-zero the empty-plate baseline (RAM only) while disarmed
    // with nobody seated. Never runs in LEAN, and entering LEAN forces a new
    // neutral calibration, so a baseline never changes under a live neutral.
    constexpr bool EMPTY_SEAT_AUTO_ZERO = true;
    leanMode.trackEmptyPlate(now, EMPTY_SEAT_AUTO_ZERO && modeState.current() == DISARM);
    {
        // 20 Hz raw ADC counts (front/back/left/right) for lean-transition analysis.
        const auto &raw = leanMode.getRawReadings();
        telemetry::Telemetry::logf("DBG R %lu %d %d %d %d\n", static_cast<unsigned long>(now), raw[0], raw[1], raw[2], raw[3]);
        // 20 Hz centre of pressure in inches (corner layout only).
        leanMode.publishCentreOfPressure(now);
    }
    telemetry::TelemetryFrame frame;
    frame.timeMs = now;
    frame.mode = modeState.current();
    frame.lean = leanMode.getTelemetry();
    frame.userWeight = leanMode.getUserWeightEstimate();
    frame.remoteStopped = remoteMode.getTelemetry().stopped;
    frame.commands = motorTelemetry;
    const auto feedback = [](const VescStatus &motor)
    {
        return telemetry::MotorFeedback{motor.erpm / VESC_ERPM_PER_WHEEL_RPM,
                                        motor.lastRpmMs, motor.rpmFrames != 0};
    };
    frame.left = feedback(measuredMotors[0]);
    frame.right = feedback(measuredMotors[1]);
    frame.boostCurrent = assistBoostCurrent;
    telemetryOutput.publish(frame);
}

void loop()
{
    updateKnobs();
    updateModeInputs();
    packetSerial.update();
    CAN_message_t feedback;
    for (int i = 0; i < 32 && can1.read(feedback); ++i)
    {
        vescHandleFrame(feedback, motors.leftId, measuredMotors[0]);
        vescHandleFrame(feedback, motors.rightId, measuredMotors[1]);
    }
    updateDrive();
    updateModeLEDs();
    updateProfileLED();
    replyToConfiguration();

    publishTelemetry();
}
