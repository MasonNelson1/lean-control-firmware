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

using namespace common;

// Runtime knob selections, set by updateKnobs(). Start at the gentlest
// position until the knobs are read in setup().
knobs::RotarySwitch speedKnob;
knobs::RotarySwitch profileKnob;
static int lastProfilePosition = knobs::Invalid;
static int pendingProfile = knobs::Invalid;
static int activeProfile = knobs::Invalid;
float userMaxSpeedMS = knobs::speedLimitMS(0);
const float userMaxAccelMS2 = knobs::accelLimitMS2(0);
// TODO: a stop/fault should bypass smoothing and reset BOTH PID instances.

// MISC CONSTANTS and vars
ezButton calibrateButton(PIN_CAL_BUTTON);
ezButton leanModeButton(PIN_LEAN_BUTTON);
ezButton remoteModeButton(PIN_REMOTE_BUTTON);
ezButton disarmModeButton(PIN_DISARM_BUTTON);

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

struct MotorCommandTelemetry
{
    float leftTargetRPM;
    float rightTargetRPM;
    float leftSmoothedRPM;
    float rightSmoothedRPM;
};

static MotorCommandTelemetry motorTelemetry{};
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
    const float leftRpms = leftRPMSetpointPID.smoothRPM(leftTargetRPM, dt, speedLimitMS, userMaxAccelMS2);
    const float rightRpms = rightRPMSetpointPID.smoothRPM(rightTargetRPM, dt, speedLimitMS, userMaxAccelMS2);
    vescSetWheelRPM(can1, motors.leftId, leftRpms);
    vescSetWheelRPM(can1, motors.rightId, rightRpms);
    motorTelemetry = {leftTargetRPM, rightTargetRPM, leftRpms, rightRpms};
}

mode MODE = REMOTE;
// Keep the selected mode while the seat latch temporarily disarms the drive.
static mode selectedMode = REMOTE;
static uint32_t assistEnteredAt = 0;
static bool calibrationPending = false;
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
const char *modeName(mode currentMode)
{
    switch (currentMode)
    {
    case LEAN:
        return "LEAN";
    case REMOTE:
        return "REMOTE";
    case DISARM:
        return "DISARM";
    case ASSIST:
        return "ASSIST";
    default:
        return "UNKNOWN";
    }
}

void printMode()
{
    Serial.printf("Mode: %s\n", modeName(MODE));
}

// Apply the knobs' current positions and report them as one text line.
// Non-CSV lines show up as console text in the debug viewer.
static void applyKnobs()
{
    userMaxSpeedMS = knobs::speedLimitMS(speedKnob.position());

    // this only prints when the knobs are moved, not all the time.
    char line[160];
    const int length = snprintf(line, sizeof(line),
                                "Knobs: speed %d/%d (%.2f m/s, raw %u)%s, profile %d/%d (raw %u)%s\n",
                                speedKnob.position() + 1, knobs::Positions, userMaxSpeedMS,
                                static_cast<unsigned>(speedKnob.lastRaw()), speedKnob.faulted() ? " FAULT->slowest" : "",
                                profileKnob.position() + 1, knobs::Positions,
                                static_cast<unsigned>(profileKnob.lastRaw()), profileKnob.faulted() ? " FAULT" : "");
    if (length <= 0 || length >= static_cast<int>(sizeof(line)))
        return;
    Serial.write(reinterpret_cast<const uint8_t *>(line), length);
    if (Serial1.availableForWrite() >= length)
        Serial1.write(reinterpret_cast<const uint8_t *>(line), length);
}

static bool activateProfile(int slot)
{
    if (!settings::protocol.selectProfile(slot))
        return false;
    activeProfile = slot;
    applyRuntimeSettings();
    leanMode.reset();
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
    static uint8_t bluetoothTxBuffer[512];

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
    selectedMode = settings::defaultMode.get();
    MODE = selectedMode;
    if (selectedMode == LEAN)
        leanMode.reset();
    applyRuntimeSettings();

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
    printMode();
    Serial.println("Setup successful, ready to drive.");
}

void setMode()
{
    constexpr uint32_t assistHoldMs = 500;
    static bool leanPressPending = false;
    static bool remotePressPending = false;
    static bool disarmPressPending = false;
    static bool assistHoldPending = false;
    static uint32_t disarmPressedAt = 0;
    static uint32_t profilePressedAt = 0;
    static int confirmingProfile = knobs::Invalid;

    noInterrupts();
    const bool leanEdge = leanEdgePending;
    const bool remoteEdge = remoteEdgePending;
    const bool disarmEdge = disarmEdgePending;
    leanEdgePending = false;
    remoteEdgePending = false;
    disarmEdgePending = false;
    interrupts();

    leanPressPending |= leanEdge;
    remotePressPending |= remoteEdge;
    disarmPressPending |= disarmEdge;

    // loop the buttons right away
    leanModeButton.loop();
    remoteModeButton.loop();
    calibrateButton.loop();
    calibrationPending |= calibrateButton.isPressed() && selectedMode == LEAN;
    disarmModeButton.loop();

    // A confirmation belongs to one stable detent and one continuous press.
    if (confirmingProfile != pendingProfile || !profileKnob.settled() ||
        leanModeButton.getStateRaw() != LOW || MODE != DISARM || selectedMode != DISARM)
        confirmingProfile = knobs::Invalid;

    if (leanPressPending || remotePressPending || disarmPressPending || assistHoldPending ||
        confirmingProfile != knobs::Invalid)
    {
        leanMode.sampleSensors(/*seatOnly=*/true);
        if (!leanMode.isSeated())
        {
            leanPressPending = false;
            remotePressPending = false;
            disarmPressPending = false;
            assistHoldPending = false;
            confirmingProfile = knobs::Invalid;
            return;
        }
    }

    const bool leanPressed = leanModeButton.isPressed() && leanPressPending;
    const bool remotePressed = remoteModeButton.isPressed() && remotePressPending;
    const bool disarmPressed = disarmModeButton.isPressed() && disarmPressPending;
     
    const uint32_t now = millis();
    if (leanPressed)
        leanPressPending = false;
    if (remotePressed)
        remotePressPending = false;
    if (disarmPressed) {
        disarmPressPending = false;
        disarmPressedAt = now;
    }
       
    if (leanModeButton.isReleased())
        leanPressPending = false;
    if (remoteModeButton.isReleased())
        remotePressPending = false;
    if (disarmModeButton.isReleased())
    {
        disarmPressPending = false;
        assistHoldPending = false;
    }

    const bool profilePress = leanPressed && pendingProfile != knobs::Invalid;
    if (profilePress && profileKnob.settled() && MODE == DISARM && selectedMode == DISARM)
    {
        confirmingProfile = pendingProfile;
        profilePressedAt = now;
    }
    if (disarmPressed || remotePressed || assistHoldPending)
        confirmingProfile = knobs::Invalid;
    if (confirmingProfile != knobs::Invalid && now - profilePressedAt >= 2000)
    {
        if (!activateProfile(confirmingProfile))
            Serial.printf("Profile %d unavailable; keeping current settings.\n", confirmingProfile + 1);
        pendingProfile = knobs::Invalid;
        confirmingProfile = knobs::Invalid;
    }

    if (disarmPressed)
    {
        // A press that exits Assist cannot reenter it until the next press.
        assistHoldPending = selectedMode != ASSIST;
        selectedMode = DISARM;
    }
    else if (leanPressed && !profilePress && selectedMode != LEAN)
    {
        assistHoldPending = false;
        selectedMode = LEAN;
    }
    else if (remotePressed && selectedMode != REMOTE)
    {
        assistHoldPending = false;
        selectedMode = REMOTE;
        remoteMode.reset();
    }
    else if (assistHoldPending &&
             now - disarmPressedAt >= assistHoldMs)
    {
        assistHoldPending = false;
        assistMode.reset();
        selectedMode = ASSIST;
    }
}

static void updateModeLEDs()
{
    constexpr uint32_t assistBlinkMs = 500;
    switch (MODE)
    {
    case LEAN:
        digitalWrite(common::PIN_LEAN_LED, HIGH);
        digitalWrite(common::PIN_DISARM_LED, LOW);
        digitalWrite(common::PIN_REMOTE_LED, LOW);
        // calibration button status is handled in the getTargets() for this mode.
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
                     ((millis() - assistEnteredAt) / assistBlinkMs) % 2 == 0 ? HIGH : LOW);
        digitalWrite(common::PIN_REMOTE_LED, LOW);
        digitalWrite(common::PIN_CAL_LED, LOW);
        break;
    default:
        break;
    }
}

static bool selectedModeRequiresSeat()
{
    return selectedMode == LEAN || selectedMode == ASSIST;
}

static void applySeatedLatch()
{
    const bool seatDisarm = selectedModeRequiresSeat() && !leanMode.isSeated();
    const mode nextMode = seatDisarm ? DISARM : selectedMode;
    if (nextMode == MODE)
        return;

    if (seatDisarm)
    {
        leftRPMSetpointPID.reset();
        rightRPMSetpointPID.reset();
        assistMode.reset();
    }
    if (nextMode == LEAN)
        leanMode.reset();
    else if (nextMode == ASSIST)
        assistEnteredAt = millis();

    MODE = nextMode;
    printMode();
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

    if (selectedModeRequiresSeat())
    {
        leanMode.sampleSensors();
        if (selectedMode == LEAN && leanMode.isSeated() && calibrationPending)
        {
            leanMode.calibrate();
            leanMode.sampleSensors();
        }
    }
    calibrationPending = false;
    applySeatedLatch();

    const float dt = period / 1000.0f;
    MotorTargets targets;
    switch (MODE)
    {
    case LEAN:
        targets = leanMode.getTargetsFromSample();
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
        Serial.println("Unknown mode detected, setting motor power to 0, this should never happen");
        break;
    }
    assistBoostCurrent = MODE == ASSIST ? targets : MotorTargets{};
    if (MODE == LEAN || MODE == REMOTE)
    {
        setMotorPower(targets.left, targets.right, dt);
    }
    else if (MODE == DISARM)
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
    settings::protocol.tick(millis(), MODE == DISARM);
    if (!settings::protocol.pending() || Serial1.availableForWrite() < static_cast<int>(MaxPacketBytes))
        return;

    uint8_t response[config::MaxResponseSize];
    // Configuration writes are permitted only while the current mode is DISARM.
    // Only successful runtime mutations require refreshing derived settings.
    const uint32_t revision = settings::protocol.revision();
    const size_t length = settings::protocol.reply(response, MODE == DISARM);
    if (settings::protocol.revision() != revision)
        applyRuntimeSettings();
    // The leading zero distinguishes PacketSerial responses from legacy CSV.
    Serial1.write(static_cast<uint8_t>(0));
    packetSerial.send(response, length);
}

void loop()
{
    updateKnobs();
    setMode();
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

    // Keep sensor telemetry at 20 Hz in every mode.
    static uint32_t lastTelemetryMs = 0;
    const uint32_t now = millis();
    if (now - lastTelemetryMs < 50)
        return;
    lastTelemetryMs = now;

    static bool headerPrinted = false;
    if (!headerPrinted)
    {
        Serial.println("time_ms,F,B,L,R,total,userWeight,speed,turn,leftTargetRPM,rightTargetRPM,"
                       "leftSmoothedRPM,rightSmoothedRPM,stopped,leftActualRPM,rightActualRPM,"
                       "leftBoostCurrent,rightBoostCurrent,mode");
        headerPrinted = true;
    }

    const bool leanActive = MODE == LEAN;
    // LEAN samples on each control tick; other modes sample for display only.
    if (!leanActive)
        leanMode.sampleSensors();
    const LeanTelemetry lean = leanMode.getTelemetry();
    const bool stopped = MODE == DISARM || (leanActive ? lean.stopped : remoteMode.getTelemetry().stopped);
    const bool rpmControl = MODE == LEAN || MODE == REMOTE;
    const MotorCommandTelemetry commands = rpmControl
        ? motorTelemetry : MotorCommandTelemetry{NAN, NAN, NAN, NAN};
    const auto actualRPM = [now](const VescStatus &motor) {
        constexpr uint32_t feedbackTimeoutMs = 500;
        return motor.rpmFrames && now - motor.lastRpmMs <= feedbackTimeoutMs
            ? motor.erpm / VESC_ERPM_PER_WHEEL_RPM : NAN;
    };
    const int total = lean.front + lean.back + lean.left + lean.right;

    char row[256];
    const int rowLength = snprintf(row, sizeof(row),
                                   "%lu,%d,%d,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%.4f,%s\n",
                                   static_cast<unsigned long>(now),
                                   lean.front, lean.back, lean.left, lean.right, total,
                                   leanMode.getUserWeightEstimate(), lean.speed, lean.turn,
                                   commands.leftTargetRPM, commands.rightTargetRPM,
                                   commands.leftSmoothedRPM, commands.rightSmoothedRPM,
                                   stopped ? 1 : 0, actualRPM(measuredMotors[0]), actualRPM(measuredMotors[1]),
                                   assistBoostCurrent.left, assistBoostCurrent.right, modeName(MODE));
    if (rowLength <= 0 || rowLength >= static_cast<int>(sizeof(row)))
        return;

    // Serial.write(reinterpret_cast<const uint8_t *>(row), rowLength);
    Serial.println(measuredMotors[1].inputVoltage);
    // Serial.println(digitalRead(common::PIN_CAL_BUTTON));

    // The HC-06 shares Serial1 with the remote controller.
    if (Serial1.availableForWrite() >= rowLength)
        Serial1.write(reinterpret_cast<const uint8_t *>(row), rowLength);
}
