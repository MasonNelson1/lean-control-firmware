#include <Arduino.h>
#include <ezButton.h>
#include "vesc_can.h"
#include "rpm_setpoint_pid.h"
#include "common.h"
#include "driveModes/DriveMode.h"
#include "driveModes/LeanDriveMode.h"
#include "driveModes/RemoteDriveMode.h"
#include "PacketSerial.h"
#include "config/DriveSettings.h"

using namespace common;

// Runtime knob selections: kept at the hard design limits until knobs are wired.
float userMaxSpeedMS = MAX_SPEED;
float userMaxAccelMS2 = MAX_ACCEL;
// Future integration (once per VESC update):
// leftRPMSetpointPID.smoothRPM(leftTargetRPM, dt, userMaxSpeedMS, userMaxAccelMS2);
// Likewise for right. A stop/fault must bypass smoothing and reset BOTH instances.

// MISC CONSTANTS and vars
ezButton calibrateButton(PIN_CAL_BUTTON);
ezButton leanModeButton(PIN_LEAN_BUTTON);
ezButton remoteModeButton(PIN_REMOTE_BUTTON);

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

struct MotorCommandTelemetry
{
    float leftTargetRPM;
    float rightTargetRPM;
    float leftSmoothedRPM;
    float rightSmoothedRPM;
};

static MotorCommandTelemetry motorTelemetry{};

// This function takes doubles from -1 to 1 for the left and right motors and sets their speeds to those values
// where the value is the percentage of max speed.
// PID smoothing is applied prior to using the setpoints.
void setMotorPower(double left, double right, float dt)
{
    const float leftTargetRPM = constrain(left, -1.0f, 1.0f) * MAX_SPEED * LINEAR_TO_RPM;
    const float rightTargetRPM = constrain(right, -1.0f, 1.0f) * MAX_SPEED * LINEAR_TO_RPM;
    const float leftRpms = leftRPMSetpointPID.smoothRPM(leftTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
    const float rightRpms = rightRPMSetpointPID.smoothRPM(rightTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
    vescSetWheelRPM(can1, motors.leftId, leftRpms);
    vescSetWheelRPM(can1, motors.rightId, rightRpms);
    motorTelemetry = {leftTargetRPM, rightTargetRPM, leftRpms, rightRpms};
}

mode MODE = REMOTE;
LeanDriveMode leanMode;

PacketSerial packetSerial;
RemoteDriveMode remoteMode;

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
    leanMode.updateResponseCurve();
    const auto tuning = settings::normalPid.tuning();
    leftRPMSetpointPID.setTuning(tuning);
    rightRPMSetpointPID.setTuning(tuning);
}

// Interrupts latch raw presses; ezButton confirms the debounced press in loop().
// Never call ezButton, print, or command motors from an interrupt handler.
static volatile bool leanEdgePending = false;
static volatile bool remoteEdgePending = false;

void onLeanButtonPress()
{
    leanEdgePending = true;
}

void onRemoteButtonPress()
{
    remoteEdgePending = true;
}

const char *modeName(mode currentMode)
{
    switch (currentMode)
    {
    case LEAN:
        return "LEAN";
    case REMOTE:
        return "REMOTE";
    default:
        return "UNKNOWN";
    }
}

void printMode()
{
    Serial.printf("Mode: %s\n", modeName(MODE));
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

    settings::persistence.load();
    MODE = settings::defaultMode.get();
    applyRuntimeSettings();

    // user input
    analogReadResolution(12);
    analogReadAveraging(4);
    // ezButton configures the pins as INPUT_PULLUP: pressing connects to GND.

    attachInterrupt(digitalPinToInterrupt(PIN_LEAN_BUTTON), onLeanButtonPress, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_REMOTE_BUTTON), onRemoteButtonPress, FALLING);

    // motors
    vescBegin(can1);
    delay(200);
    printMode();
    Serial.println("Setup successful, ready to drive.");
}

void setMode()
{

    static bool leanPressPending = false;
    static bool remotePressPending = false;

    noInterrupts();
    const bool leanEdge = leanEdgePending;
    const bool remoteEdge = remoteEdgePending;
    leanEdgePending = false;
    remoteEdgePending = false;
    interrupts();

    leanPressPending |= leanEdge;
    remotePressPending |= remoteEdge;

    // loop the buttons right away
    leanModeButton.loop();
    remoteModeButton.loop();
    calibrateButton.loop();

    const bool leanPressed = leanModeButton.isPressed() && leanPressPending;
    const bool remotePressed = remoteModeButton.isPressed() && remotePressPending;
    if (leanPressed)
        leanPressPending = false;
    if (remotePressed)
        remotePressPending = false;
    if (leanModeButton.isReleased())
        leanPressPending = false;
    if (remoteModeButton.isReleased())
        remotePressPending = false;

    if (leanPressed && (MODE != LEAN))
    {
        MODE = LEAN;
        printMode();
    }
    else if (remotePressed && (MODE != REMOTE))
    {
        MODE = REMOTE;
        remoteMode.reset();
        printMode();
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

    const float dt = period / 1000.0f;
    const MotorTargets targets = MODE == LEAN ? leanMode.getTargets() : remoteMode.getTargets();
    setMotorPower(targets.left, targets.right, dt);
}

static void replyToConfiguration()
{
    constexpr size_t MaxPacketBytes = config::MaxResponseSize + config::MaxResponseSize / 254 + 3;
    if (!settings::protocol.pending() || Serial1.availableForWrite() < static_cast<int>(MaxPacketBytes))
        return;

    uint8_t response[config::MaxResponseSize];
    // The protocol checks the latest smoothed wheel commands before writes.
    // Only successful runtime mutations require refreshing derived settings.
    const uint32_t revision = settings::protocol.revision();
    const size_t length = settings::protocol.reply(response, motorTelemetry.leftSmoothedRPM,
                                                   motorTelemetry.rightSmoothedRPM);
    if (settings::protocol.revision() != revision)
        applyRuntimeSettings();
    // The leading zero distinguishes PacketSerial responses from legacy CSV.
    Serial1.write(static_cast<uint8_t>(0));
    packetSerial.send(response, length);
}

void loop()
{
    setMode();
    packetSerial.update();
    if (calibrateButton.isPressed() && MODE == LEAN)
        leanMode.calibrate();

    updateDrive();
    replyToConfiguration();

    // Keep telemetry at 20 Hz in both modes.
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
    const LeanTelemetry lean = leanActive ? leanMode.getTelemetry() : LeanTelemetry{};
    const bool stopped = leanActive ? lean.stopped : remoteMode.getTelemetry().stopped;
    const int total = lean.front + lean.back + lean.left + lean.right;

    char row[256];
    const int rowLength = snprintf(row, sizeof(row),
                                   "%lu,%d,%d,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%s\n",
                                   static_cast<unsigned long>(now),
                                   lean.front, lean.back, lean.left, lean.right, total,
                                   leanMode.getUserWeightEstimate(), lean.speed, lean.turn,
                                   motorTelemetry.leftTargetRPM, motorTelemetry.rightTargetRPM,
                                   motorTelemetry.leftSmoothedRPM, motorTelemetry.rightSmoothedRPM,
                                   stopped ? 1 : 0, modeName(MODE));
    if (rowLength <= 0 || rowLength >= static_cast<int>(sizeof(row)))
        return;

    Serial.write(reinterpret_cast<const uint8_t *>(row), rowLength);

    // The HC-06 shares Serial1 with the remote controller.
    if (leanActive && Serial1.availableForWrite() >= rowLength)
        Serial1.write(reinterpret_cast<const uint8_t *>(row), rowLength);
}
