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
knobs::RotarySwitch accelKnob;
float userMaxSpeedMS = knobs::speedLimitMS(0);
float userMaxAccelMS2 = knobs::accelLimitMS2(0);
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

struct WheelCommandTelemetry
{
    float targetRPM;
    float smoothedRPM;
};

struct MotorCommandTelemetry
{
    WheelCommandTelemetry left;
    WheelCommandTelemetry right;
};

static MotorCommandTelemetry motorTelemetry{};

static bool leftNeedsVelocitySeed = false;
static bool rightNeedsVelocitySeed = false;

static float measuredWheelRPM(const VescStatus &motor)
{
    return motor.erpm / VESC_ERPM_PER_WHEEL_RPM;
}

static WheelCommandTelemetry updateWheel(uint8_t motorId, const VescStatus &feedback,
                                         RPMSetpointPID &smoother, bool &needsVelocitySeed,
                                         LeanDriveMode::WheelCommand command, float speedLimitMS,
                                         float dt)
{
    if (command.currentControl)
    {
        needsVelocitySeed = true;
        vescSetCurrent(can1, motorId, command.value);
        return {};
    }

    if (needsVelocitySeed)
    {
        smoother.seedRPM(measuredWheelRPM(feedback));
        needsVelocitySeed = false;
    }
    const float targetRPM = constrain(command.value, -1.0f, 1.0f) * speedLimitMS * LINEAR_TO_RPM;
    const float smoothedRPM = smoother.smoothRPM(targetRPM, dt, speedLimitMS, userMaxAccelMS2);
    vescSetWheelRPM(can1, motorId, smoothedRPM);
    return {targetRPM, smoothedRPM};
}

static void setMotorPower(const LeanDriveMode::WheelCommands &commands, float dt)
{
    // Turning the speed knob up takes effect at once (the PID still limits
    // acceleration). Turning it down lowers the cap no faster than the design
    // braking rate, so the smoother never sees a step drop in its speed limit.
    static float speedLimitMS = 0.0f;
    speedLimitMS = knobs::slewDown(speedLimitMS, userMaxSpeedMS, MAX_ACCEL, dt);

    motorTelemetry.left = updateWheel(motors.leftId, measuredMotors[0], leftRPMSetpointPID,
                                     leftNeedsVelocitySeed, commands.left, speedLimitMS, dt);
    motorTelemetry.right = updateWheel(motors.rightId, measuredMotors[1], rightRPMSetpointPID,
                                      rightNeedsVelocitySeed, commands.right, speedLimitMS, dt);
}

mode MODE = REMOTE;
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
    userMaxAccelMS2 = knobs::accelLimitMS2(accelKnob.position());

    // this only prints when the knobs are moved, not all the time.
    char line[160];
    const int length = snprintf(line, sizeof(line),
                                "Knobs: speed %d/%d (%.2f m/s, raw %u)%s, accel %d/%d (%.2f m/s2, raw %u)%s\n",
                                speedKnob.position() + 1, knobs::Positions, userMaxSpeedMS,
                                static_cast<unsigned>(speedKnob.lastRaw()), speedKnob.faulted() ? " FAULT->slowest" : "",
                                accelKnob.position() + 1, knobs::Positions, userMaxAccelMS2,
                                static_cast<unsigned>(accelKnob.lastRaw()), accelKnob.faulted() ? " FAULT->slowest" : "");
    if (length <= 0 || length >= static_cast<int>(sizeof(line)))
        return;
    Serial.write(reinterpret_cast<const uint8_t *>(line), length);
    if (Serial1.availableForWrite() >= length)
        Serial1.write(reinterpret_cast<const uint8_t *>(line), length);
}

// Sample both knobs every knobs::SampleMs; an invalid (open/shorted) knob
// falls back to its slowest position after knobs::FaultHoldMs.
static void updateKnobs()
{
    static uint32_t lastSampleMs = 0;
    const uint32_t now = millis();
    if (now - lastSampleMs < knobs::SampleMs)
        return;
    lastSampleMs = now;

    const bool speedChanged = speedKnob.update(analogRead(PIN_SPEED), now);
    const bool accelChanged = accelKnob.update(analogRead(PIN_ACCEL), now);
    if (speedChanged || accelChanged)
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
    MODE = settings::defaultMode.get();
    if (MODE == LEAN)
        leanMode.reset();
    applyRuntimeSettings();

    // user input
    analogReadResolution(12);
    analogReadAveraging(4);
    // Take the knobs' power-on positions before the first drive tick. An
    // invalid reading leaves that knob at its slowest position.
    speedKnob.seed(analogRead(PIN_SPEED), millis());
    accelKnob.seed(analogRead(PIN_ACCEL), millis());
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
    constexpr uint32_t assistBlinkMs = 500;
    static bool leanPressPending = false;
    static bool remotePressPending = false;
    static bool disarmPressPending = false;
    static bool assistHoldPending = false;
    static uint32_t disarmPressedAt = 0;
    static uint32_t assistEnteredAt = 0;

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
    disarmModeButton.loop();

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

    if (disarmPressed)
    {
        // A press that exits Assist cannot reenter it until the next press.
        assistHoldPending = MODE != ASSIST;
        if (MODE != DISARM)
        {
            MODE = DISARM;
            printMode();
        }
    }
    else if (leanPressed && (MODE != LEAN))
    {
        assistHoldPending = false;
        MODE = LEAN;
        leanMode.reset();
        printMode();
    }
    else if (remotePressed && (MODE != REMOTE))
    {
        assistHoldPending = false;
        MODE = REMOTE;
        remoteMode.reset();
        printMode();
    }
    else if (assistHoldPending &&
             now - disarmPressedAt >= assistHoldMs)
    {
        assistHoldPending = false;
        assistEnteredAt = now;
        assistMode.reset();
        MODE = ASSIST;
        printMode();
    }

    // set mode LED's
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
                     ((now - assistEnteredAt) / assistBlinkMs) % 2 == 0 ? HIGH : LOW);
        digitalWrite(common::PIN_REMOTE_LED, LOW);
        digitalWrite(common::PIN_CAL_LED, LOW);
        break;
    default:
        break;
    }
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

    if (MODE != LEAN && MODE != REMOTE)
    {
        leftNeedsVelocitySeed = false;
        rightNeedsVelocitySeed = false;
    }

    const float dt = period / 1000.0f;
    MotorTargets targets;
    switch (MODE)
    {
    case LEAN:
        setMotorPower(leanMode.getWheelCommands(measuredWheelRPM(measuredMotors[0]),
                                               measuredWheelRPM(measuredMotors[1]),
                                               userMaxSpeedMS * LINEAR_TO_RPM, now), dt);
        return;

    case REMOTE:
        targets = remoteMode.getTargets();
        setMotorPower({{targets.left}, {targets.right}}, dt);
        return;
    case DISARM:
        targets = disarmMode.getTargets();
        break;
    case ASSIST:
        assistMode.loop(measuredWheelRPM(measuredMotors[0]), measuredWheelRPM(measuredMotors[1]),
                        userMaxSpeedMS * LINEAR_TO_RPM);
        targets = assistMode.getTargets();
        break;
    default:
        targets = {0.0f, 0.0f};
        Serial.println("Unknown mode detected, setting motor power to 0, this should never happen");
        break;
    }
    if (MODE == DISARM)
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
    setMode();
    updateKnobs();
    packetSerial.update();
    CAN_message_t feedback;
    for (int i = 0; i < 32 && can1.read(feedback); ++i)
    {
        vescHandleFrame(feedback, motors.leftId, measuredMotors[0]);
        vescHandleFrame(feedback, motors.rightId, measuredMotors[1]);
    }
    if (calibrateButton.isPressed() && MODE == LEAN)
        leanMode.calibrate();

    updateDrive();
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
                       "leftSmoothedRPM,rightSmoothedRPM,stopped,mode");
        headerPrinted = true;
    }

    const bool leanActive = MODE == LEAN;
    // LEAN samples on each control tick; other modes sample for display only.
    if (!leanActive)
        leanMode.sampleSensors();
    const LeanTelemetry lean = leanMode.getTelemetry();
    const bool stopped = MODE == DISARM || (leanActive ? lean.stopped : remoteMode.getTelemetry().stopped);
    const MotorCommandTelemetry commands = MODE == DISARM ? MotorCommandTelemetry{} : motorTelemetry;
    const int total = lean.front + lean.back + lean.left + lean.right;

    char row[256];
    const int rowLength = snprintf(row, sizeof(row),
                                   "%lu,%d,%d,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%s\n",
                                   static_cast<unsigned long>(now),
                                   lean.front, lean.back, lean.left, lean.right, total,
                                   leanMode.getUserWeightEstimate(), lean.speed, lean.turn,
                                   commands.left.targetRPM, commands.right.targetRPM,
                                   commands.left.smoothedRPM, commands.right.smoothedRPM,
                                   stopped ? 1 : 0, modeName(MODE));
    if (rowLength <= 0 || rowLength >= static_cast<int>(sizeof(row)))
        return;

    Serial.write(reinterpret_cast<const uint8_t *>(row), rowLength);
    // Serial.println(digitalRead(common::PIN_CAL_BUTTON));

    // The HC-06 shares Serial1 with the remote controller.
    if (Serial1.availableForWrite() >= rowLength)
        Serial1.write(reinterpret_cast<const uint8_t *>(row), rowLength);
}
