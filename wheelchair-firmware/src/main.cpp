#include <Arduino.h>
#include <ezButton.h>
#include "vesc_can.h"
#include "vesc_probe.h"
#include "vesc_apply.h"
#include "vesc_monitor.h"
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

// Safety brake (Charles, court 2026-11-22): an AUTOMATIC disarm (sensor fault,
// CoP jump, empty seat) used to send zero current, so a moving chair coasted while
// the rider thought he was still in LEAN. Now an automatic disarm latches this
// brake: the DISARM LED blinks and both VESCs get brake current (which slows the
// wheel toward zero and can't reverse it) until the rider presses DISARM (then the
// wheels are free, as before) or selects another mode.
static bool safetyBrake = false;
// All four mode LEDs blink for a moment whenever a setting changes over Bluetooth
// (dashboard or Claude's CLI), so the riders can see a change land (Charles, 2026-11-22).
static uint32_t settingsFlashUntil = 0;
constexpr uint32_t SETTINGS_FLASH_MS = 1200; // ~4 blinks at 150 ms
constexpr float SAFETY_BRAKE_MOVING_A = 15.0f; // VESC braking current limit is 20 A
constexpr float SAFETY_BRAKE_HOLD_A = 5.0f;    // once both wheels are below SAFETY_BRAKE_STILL_RPM
constexpr float SAFETY_BRAKE_STILL_RPM = 3.0f;
static void latchSafetyBrake(const char *why)
{
    if (!safetyBrake)
        telemetry::Telemetry::logf("DBG EVT %lu brake-latch on why=%s\n", static_cast<unsigned long>(millis()), why);
    safetyBrake = true;
}
static void releaseSafetyBrake(const char *why)
{
    if (safetyBrake)
        telemetry::Telemetry::logf("DBG EVT %lu brake-latch off why=%s\n", static_cast<unsigned long>(millis()), why);
    safetyBrake = false;
}


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

// Timing and link counters for the once-a-second DBG SYS line (telemetry only).
static struct
{
    uint32_t loops = 0, loopMaxUs = 0;        // since the last DBG SYS
    uint32_t ctrlMaxMs = 0, ctrlMinMs = ~0u;  // control tick spacing since the last DBG SYS
    uint32_t canRx = 0;                       // CAN frames read since boot
} sysStats;
static uint32_t configReplyBytes = 0; // configuration replies sent over Bluetooth since boot

static void noteControlTick(uint32_t gapMs)
{
    sysStats.ctrlMaxMs = max(sysStats.ctrlMaxMs, gapMs);
    sysStats.ctrlMinMs = min(sysStats.ctrlMinMs, gapMs);
}

// This function takes doubles from -1 to 1 for the left and right motors and sets their speeds to those values
// where the value is the percentage of the speed-knob max speed (full input = selected top speed).
// PID smoothing is applied prior to using the setpoints.
// Speed cap after the slew below (m/s); file scope only so DBG M can log it.
static float speedLimitMS = 0.0f;

void setMotorPower(double left, double right, float dt)
{
    // Turning the speed knob up takes effect at once (the PID still limits
    // acceleration). Turning it down lowers the cap no faster than the design
    // braking rate, so the smoother never sees a step drop in its speed limit.
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
    telemetry::Telemetry::logf("DBG EVT %lu profile slot=%d\n", static_cast<unsigned long>(millis()), slot + 1);
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
    // Once-a-second lines each have their own 20 Hz tick (publishTelemetry), so
    // the largest single tick is a 20 Hz group (CSV ≈95, DBG R ≈30, COP ≈35,
    // M ≈75) plus DBG VVAL ≈260: ≈ 500 bytes. A 50 ms tick drains ≈ 576 bytes
    // at 115200 baud. Telemetry drops whole lines rather than block when this is
    // full and counts them (DBG SYS drop=).
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

// DBG EVT for every debounced press and release, accepted by the mode state
// machine or not (a press with the seat empty is discarded there).
static void logButtonEvents(const ModeStateMachine::Inputs &input)
{
    const struct
    {
        const char *name;
        const ModeStateMachine::Button &button;
    } buttons[] = {{"lean", input.lean}, {"remote", input.remote}, {"disarm", input.disarm}, {"cal", input.calibrate}};
    for (const auto &b : buttons)
        if (b.button.pressed || b.button.released)
            telemetry::Telemetry::logf("DBG EVT %lu btn %s %s\n", static_cast<unsigned long>(millis()), b.name,
                                       b.button.pressed ? "down" : "up");
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
    if (input.disarm.pressed && modeState.current() == DISARM)
        releaseSafetyBrake("disarm-press");
    input.calibrate.pressed = calibrateButton.isPressed();
    if (input.calibrate.pressed)
        calibrationPressedAt = millis();
    input.calibrate.released = calibrateButton.isReleased();
    input.leanHeld = leanModeButton.getStateRaw() == LOW;
    input.profileSettled = profileKnob.settled();
    input.pendingProfile = pendingProfile;
    input.calibrationActive = leanMode.calibrationActive();
    logButtonEvents(input);
    const mode selectedBefore = modeState.selected();
    // Refresh only the seat reading, leaving the control/telemetry sample intact.
    if (modeState.needsSeatSample(input))
        leanMode.sampleSensors(/*seatOnly=*/true);
    input.seated = leanMode.isSeated();
    const auto actions = modeState.update(input, millis());
    if (modeState.selected() != selectedBefore)
        telemetry::Telemetry::logf("DBG EVT %lu select %s->%s cur=%s seated=%d\n", static_cast<unsigned long>(millis()),
                                   modeName(selectedBefore), modeName(modeState.selected()),
                                   modeName(modeState.current()), input.seated ? 1 : 0);
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
    if (static_cast<int32_t>(settingsFlashUntil - millis()) > 0)
    {
        const uint8_t on = (millis() / 150) % 2 ? HIGH : LOW;
        digitalWrite(common::PIN_LEAN_LED, on);
        digitalWrite(common::PIN_DISARM_LED, on);
        digitalWrite(common::PIN_REMOTE_LED, on);
        digitalWrite(common::PIN_CAL_LED, on);
        return;
    }
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
        // Blinking = the safety brake is holding the wheels; steady = wheels free.
        digitalWrite(common::PIN_DISARM_LED, safetyBrake ? ((millis() / 200) % 2 ? HIGH : LOW) : HIGH);
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
    if (transition.seatDisarm)
        latchSafetyBrake("seat-empty");
    if (modeState.current() != DISARM)
        releaseSafetyBrake("mode");
    telemetry::Telemetry::logf("Mode: %s\n", modeName(modeState.current()));
    telemetry::Telemetry::logf("DBG EVT %lu mode %s sel=%s why=%s\n", static_cast<unsigned long>(millis()),
                               modeName(modeState.current()), modeName(modeState.selected()),
                               transition.seatDisarm ? "seat-empty" : "selected");
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
    noteControlTick(now - tSend);
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
        const auto &raw = leanMode.getRawReadings();
        telemetry::Telemetry::logf("DBG EVT %lu rail-disarm raw=%d/%d/%d/%d\n", static_cast<unsigned long>(now),
                                   raw[0], raw[1], raw[2], raw[3]);
        latchSafetyBrake("rail");
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
        telemetry::Telemetry::logf("DBG EVT %lu cal-press held=%lu\n", static_cast<unsigned long>(now),
                                   static_cast<unsigned long>(now - calibrationPressedAt));
        leanMode.beginCalibration(calibrationPressedAt);
        leanMode.sampleSensors();
        // Calibration updates the seat threshold as well as the neutral floors.
        applySeatedLatch();
    }
    if (leanMode.calibrationActive())
    {
        if (leanMode.updateCalibration(millis(), calibrateButton.getState() == LOW))
        {
            telemetry::Telemetry::logf("DBG EVT %lu cal-end disarm\n", static_cast<unsigned long>(millis()));
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
            const auto &raw = leanMode.getRawReadings();
            telemetry::Telemetry::logf("DBG EVT %lu cop-jump raw=%d/%d/%d/%d\n", static_cast<unsigned long>(now),
                                       raw[0], raw[1], raw[2], raw[3]);
            latchSafetyBrake("cop-jump");
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
    else if (modeState.current() == DISARM && safetyBrake)
    {
        const bool still = fabsf(measuredMotors[0].erpm / VESC_ERPM_PER_WHEEL_RPM) < SAFETY_BRAKE_STILL_RPM &&
                           fabsf(measuredMotors[1].erpm / VESC_ERPM_PER_WHEEL_RPM) < SAFETY_BRAKE_STILL_RPM;
        const float amps = still ? SAFETY_BRAKE_HOLD_A : SAFETY_BRAKE_MOVING_A;
        vescSetBrakeCurrent(can1, motors.leftId, amps);
        vescSetBrakeCurrent(can1, motors.rightId, amps);
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
    {
        applyRuntimeSettings();
        settingsFlashUntil = millis() + SETTINGS_FLASH_MS;
        telemetry::Telemetry::logf("DBG EVT %lu settings rev=%lu\n", static_cast<unsigned long>(millis()),
                                   static_cast<unsigned long>(settings::protocol.revision()));
    }
    // The leading zero distinguishes PacketSerial responses from legacy CSV.
    Serial1.write(static_cast<uint8_t>(0));
    packetSerial.send(response, length);
    configReplyBytes += length + length / 254 + 3; // COBS-framed, approximately
}

// Read-only VESC firmware-version probe (see vesc_probe.h).
static vescprobe::Reply probeReply;
static vescapply::Applier vescApplier; // one-shot approved VESC settings write (vesc_apply.h)
static vescmon::Monitor vescMonitor;   // read-only GET_VALUES poll + fault watch (vesc_monitor.h)
static uint32_t probeAsked = 0, probeFrames = 0, probeReplies = 0;
// A config reply being streamed out as hex, a chunk per telemetry tick.
static uint8_t confDump[1024];
static uint16_t confDumpLen = 0, confDumpOff = 0;
static uint8_t confDumpId = 0;

static void logProbeReply()
{
    const auto &r = probeReply;
    if (r.len >= 3 && r.data[0] == vescprobe::COMM_FW_VERSION)
    {
        char hw[33] = {0};
        for (uint16_t i = 3, j = 0; i < r.len && j < sizeof(hw) - 1 && r.data[i]; ++i, ++j)
            hw[j] = static_cast<char>(r.data[i]);
        telemetry::Telemetry::logf("DBG VESCFW id=%u fw=%u.%02u hw=%s len=%u crc=%d\n", r.fromId, r.data[1], r.data[2], hw, r.len, r.crcOk);
    }
    else if (r.crcOk && r.len > 0 && (r.data[0] == vescprobe::COMM_GET_MCCONF || r.data[0] == vescprobe::COMM_GET_APPCONF) &&
             confDumpOff >= confDumpLen && r.len <= sizeof(confDump))
    {
        memcpy(confDump, r.data, r.len);
        confDumpLen = r.len;
        confDumpOff = 0;
        confDumpId = r.fromId;
    }
    else
        telemetry::Telemetry::logf("DBG VESCFW id=%u cmd=%u len=%u crc=%d\n", r.fromId, r.len ? r.data[0] : 255, r.len, r.crcOk);
}

// One 24-byte hex chunk of the pending config dump (decoded by tools/vesc_conf.py).
static void streamConfDump()
{
    if (confDumpOff >= confDumpLen)
        return;
    char hex[49];
    const uint16_t n = min<uint16_t>(24, confDumpLen - confDumpOff);
    for (uint16_t i = 0; i < n; ++i)
        snprintf(hex + 2 * i, 3, "%02x", confDump[confDumpOff + i]);
    hex[2 * n] = 0;
    telemetry::Telemetry::logf("DBG VCONF id=%u cmd=%u len=%u off=%u %s\n", confDumpId, confDump[0], confDumpLen, confDumpOff, hex);
    confDumpOff += n;
}

// 20 Hz: what each VESC reports and what the RPM smoother is doing.
//   DBG M ms curL curR dutyL dutyR iinL iinR pidL pidR accL accR rateL rateR intL intR limit
// cur/duty/iin from STATUS_1/STATUS_4 (A, A). pid = the smoother's own RPM
// before the clamp to measured ± MAX_RPM_COMMAND_DIFF (the CSV's smoothed RPM
// is after it), acc and rate in RPM/s, int in RPM·s, limit = slewed speed cap m/s.
static void publishMotorLine(uint32_t now)
{
    const VescStatus &l = measuredMotors[0], &r = measuredMotors[1];
    const RPMSetpointPID &pl = leftRPMSetpointPID, &pr = rightRPMSetpointPID;
    telemetry::Telemetry::logf("DBG M %lu %.1f %.1f %.3f %.3f %.1f %.1f %.1f %.1f %.0f %.0f %.0f %.0f %.1f %.1f %.2f\n",
                               static_cast<unsigned long>(now), l.current, r.current, l.duty, r.duty,
                               l.inputCurrent, r.inputCurrent, pl.rpm(), pr.rpm(), pl.acceleration(),
                               pr.acceleration(), pl.filteredRate(), pr.filteredRate(), pl.integral(),
                               pr.integral(), speedLimitMS);
}

// 1 Hz: the Teensy itself. loopMax µs and loops since the last line; ctrl =
// control tick spacing min/max ms; cpuC = die temperature; canRx frames and
// canTxFail refused command frames since boot; tec/rec = CAN error counters;
// bt = Bluetooth bytes/lines written since boot, drop = bytes/lines dropped
// for lack of TX space, btFree = least TX space seen this second, cfg =
// configuration reply bytes.
static void publishSystemLine(uint32_t now)
{
    const auto &link = telemetry::Telemetry::link();
    const uint32_t ecr = FLEXCAN1_ECR; // read-only error counters
    telemetry::Telemetry::logf("DBG SYS up=%lu loops=%lu loopMax=%lu ctrl=%lu/%lu cpuC=%.1f canRx=%lu canTxFail=%lu "
                               "tec=%lu rec=%lu bt=%lu/%lu drop=%lu/%lu btFree=%d cfg=%lu\n",
                               static_cast<unsigned long>(now / 1000), static_cast<unsigned long>(sysStats.loops),
                               static_cast<unsigned long>(sysStats.loopMaxUs),
                               static_cast<unsigned long>(sysStats.ctrlMinMs == ~0u ? 0 : sysStats.ctrlMinMs),
                               static_cast<unsigned long>(sysStats.ctrlMaxMs), tempmonGetTemp(),
                               static_cast<unsigned long>(sysStats.canRx), static_cast<unsigned long>(vescTxFailures()),
                               static_cast<unsigned long>(ecr & 0xFF), static_cast<unsigned long>((ecr >> 8) & 0xFF),
                               static_cast<unsigned long>(link.bytes), static_cast<unsigned long>(link.lines),
                               static_cast<unsigned long>(link.droppedBytes), static_cast<unsigned long>(link.droppedLines),
                               telemetry::Telemetry::takeMinFree(), static_cast<unsigned long>(configReplyBytes));
    sysStats.loops = sysStats.loopMaxUs = sysStats.ctrlMaxMs = 0;
    sysStats.ctrlMinMs = ~0u;
}

// 1 Hz: knobs (position 1-4 / raw ADC / S settled, F faulted, - settling),
// buttons as raw levels (1 = held: lean remote disarm cal), the profile knob's
// pending and active slots (0 = none), selected and current mode, seat.
static void publishInputsLine()
{
    const auto knob = [](const knobs::RotarySwitch &k) { return k.faulted() ? 'F' : k.settled() ? 'S' : '-'; };
    const auto held = [](ezButton &b) { return b.getStateRaw() == LOW ? 1 : 0; };
    telemetry::Telemetry::logf("DBG IO spd=%d/%u/%c prof=%d/%u/%c btn=%d%d%d%d pend=%d act=%d sel=%s cur=%s seated=%d\n",
                               speedKnob.position() + 1, static_cast<unsigned>(speedKnob.lastRaw()), knob(speedKnob),
                               profileKnob.position() + 1, static_cast<unsigned>(profileKnob.lastRaw()), knob(profileKnob),
                               held(leanModeButton), held(remoteModeButton), held(disarmModeButton), held(calibrateButton),
                               pendingProfile + 1, activeProfile + 1, modeName(modeState.selected()),
                               modeName(modeState.current()), leanMode.isSeated() ? 1 : 0);
}

// Once-a-second lines go out on their own 20 Hz tick (slot = tick % 20) so no
// tick sends more than ≈ 500 bytes. Sent together (≈ 700 bytes in one burst)
// the HC-06 lost the tail of the burst: in the 2026-11-22 tap the last line of
// it (DBG VESC) arrived intact only 82-88% of the time against ≈ 99.6% for the
// 20 Hz lines. The config dump streams on odd ticks, which carry no slot line.
static void publishTelemetry()
{
    const uint32_t now = millis();
    if (!telemetryOutput.due(now))
        return;
    static uint32_t tick = 0;
    const uint32_t slot = tick++ % 20;
    switch (slot)
    {
    case 0:
        leanMode.publishDynamicGains();
        leanMode.publishSensorStatus();
        break;
    case 2:
        leanMode.publishCornerStatus();
        break;
    case 4:
    {
        // Every 5 s one read-only GET to one VESC, cycling: firmware version,
        // motor config, app config; left then right. A full cycle takes 30 s.
        // The monitor sends it when no other reply is in flight.
        static uint8_t probeTick = 0;
        if (++probeTick % 5 == 0 && !vescApplier.active())
        {
            static const uint8_t comms[3] = {vescprobe::COMM_FW_VERSION, vescprobe::COMM_GET_MCCONF, vescprobe::COMM_GET_APPCONF};
            const uint8_t probeSlot = (probeTick / 5) % 6;
            vescMonitor.queueProbe(probeSlot < 3 ? motors.leftId : motors.rightId, comms[probeSlot % 3]);
            ++probeAsked;
        }
        telemetry::Telemetry::logf("DBG VESCPROBE asked=%lu frames=%lu replies=%lu\n", static_cast<unsigned long>(probeAsked),
                                   static_cast<unsigned long>(probeFrames), static_cast<unsigned long>(probeReplies));
        publishSystemLine(now);
        break;
    }
    case 6:
    case 8:
        // Motor controller health from the CAN status frames (vesc_monitor.h).
        vescmon::logStatus(measuredMotors[slot == 6 ? 0 : 1], slot == 6 ? motors.leftId : motors.rightId,
                           slot == 6 ? 'L' : 'R', now);
        break;
    case 10:
    case 12:
        vescMonitor.logValues(slot == 10 ? 0 : 1, now); // COMM_GET_VALUES, fault code included
        break;
    case 14:
        publishInputsLine();
        break;
    default:
        if (slot % 2)
            streamConfDump();
        break;
    }
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
    publishMotorLine(now);
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
    static uint32_t loopStartUs = micros();
    const uint32_t loopUs = micros() - loopStartUs; // the previous loop, start to start
    loopStartUs += loopUs;
    sysStats.loopMaxUs = max(sysStats.loopMaxUs, loopUs);
    ++sysStats.loops;

    updateKnobs();
    updateModeInputs();
    packetSerial.update();
    CAN_message_t feedback;
    for (int i = 0; i < 32 && can1.read(feedback); ++i)
    {
        ++sysStats.canRx;
        vescHandleFrame(feedback, motors.leftId, measuredMotors[0]);
        vescHandleFrame(feedback, motors.rightId, measuredMotors[1]);
        if (feedback.flags.extended && (feedback.id & 0xFF) == vescprobe::PROBE_CAN_ID)
            ++probeFrames;
        if (vescprobe::handleFrame(feedback, probeReply))
        {
            ++probeReplies;
            if (vescApplier.active())
                vescApplier.onReply(probeReply);
            else if (!vescMonitor.onReply(probeReply, millis()))
                logProbeReply();
            probeReply.done = false;
        }
    }
    vescMonitor.flushFaults(); // a DBG VFAULT goes out as soon as there is room
    {
        const bool disarmed = modeState.current() == DISARM;
        // The applier starts only when no monitor/probe reply is in flight; while it
        // runs, the monitor asks nothing (both share the reply path to PROBE_CAN_ID).
        vescApplier.maybeStart(vescFresh(measuredMotors[0], 500) && vescFresh(measuredMotors[1], 500) &&
                                   !vescMonitor.busy(millis()),
                               disarmed, motors.leftId, motors.rightId);
        vescApplier.update(can1, disarmed);
        vescApplier.repeatSummary();
        vescMonitor.update(can1, millis(), motors.leftId, motors.rightId, !vescApplier.active());
    }
    updateDrive();
    updateModeLEDs();
    updateProfileLED();
    replyToConfiguration();

    publishTelemetry();
}
