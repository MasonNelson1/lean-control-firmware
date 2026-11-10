#include <Arduino.h>
#include <ezButton.h>
#include "vesc_can.h"
#include "rpm_setpoint_pid.h"
#include "common.h"
#include "driveModes/DriveMode.h"
#include "driveModes/LeanDriveMode.h"
#include "driveModes/RemoteDriveMode.h"
#include "PacketSerial.h"

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

// ---------- Normal operation tuning  ----------
const RPMSetpointPID::Tuning RPM_PID_TUNING_NORM = {
    RPM_PID_LOW_SPEED_TAU_S, RPM_PID_HIGH_SPEED_TAU_S,
    RPM_PID_KI, RPM_PID_KD, RPM_PID_DERIVATIVE_FILTER_S,
    RPM_PID_LOW_SPEED_ACCEL_MS2, RPM_PID_HIGH_SPEED_ACCEL_MS2,
    RPM_PID_MAX_JERK_RPM_PER_S2};

// todo make this different.
const RPMSetpointPID::Tuning RPM_PID_TUNING_TRANSITION = {
    RPM_PID_LOW_SPEED_TAU_S, RPM_PID_HIGH_SPEED_TAU_S,
    RPM_PID_KI, RPM_PID_KD, RPM_PID_DERIVATIVE_FILTER_S,
    RPM_PID_LOW_SPEED_ACCEL_MS2, RPM_PID_HIGH_SPEED_ACCEL_MS2,
    RPM_PID_MAX_JERK_RPM_PER_S2};

RPMSetpointPID leftRPMSetpointPID(RPM_PID_TUNING_NORM);
RPMSetpointPID rightRPMSetpointPID(RPM_PID_TUNING_NORM);

// all this vesc and can stuff must be in the same file because of how can works internally.
VescStatus vescs[2] = {{VESC_LEFT_ID}, {VESC_RIGHT_ID}};

VescBus can1;

// This function takes doubles from -1 to 1 for the left and right motors and sets their speeds to those values
// where the value is the percentage of max speed.
// PID smoothing is applied prior to using the setpoints.
void setMotorPower(double left, double right)
{
    float leftTargetRPM = constrain(left, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);
    float rightTargetRPM = constrain(right, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);
    float leftRpms = leftRPMSetpointPID.smoothRPM(leftTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
    float rightRpms = rightRPMSetpointPID.smoothRPM(rightTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
    vescSetWheelRPM(can1, VESC_LEFT_ID, leftRpms);
    vescSetWheelRPM(can1, VESC_RIGHT_ID, rightRpms);
}

int unknownId = -1; // a VESC on the bus that isn't LEFT or RIGHT

mode MODE = LEAN;
LeanDriveMode leanMode;

PacketSerial packetSerial;
RemoteDriveMode remoteMode(packetSerial);

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
    // set up serial and telemetry stuff
    Serial.begin(115200);
    Serial1.begin(115200);
    packetSerial.setStream(&Serial1);

    // user input
    analogReadResolution(12);
    analogReadAveraging(4);
    calibrateButton.setDebounceTime(BUTTON_DEBOUNCE);
    leanModeButton.setDebounceTime(BUTTON_DEBOUNCE);
    remoteModeButton.setDebounceTime(BUTTON_DEBOUNCE);
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

    // todo possibly implement a different pid controller when switching modes.
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

void loop()
{
    setMode();
    packetSerial.update();

    if (calibrateButton.isPressed() && MODE == LEAN)
    {
        leanMode.calibrate();
    }
    // rate limit the loop.
    // this is placed right before the motor communication and pid smoothing because pid needs to run at a fixed rate.
    static uint32_t tSend = 0;
    if (millis() - tSend < CONTROL_RATE)
    {
        return;
    }
    tSend = millis();

    // // --- receive VESC status (not used for control) ---
    // CAN_message_t msg;
    // while (can1.read(msg))
    // {
    //     vescHandleFrame(msg, vescs, 2);
    //     uint8_t id = msg.id & 0xFF, type = (msg.id >> 8) & 0xFF;
    //     if (msg.flags.extended && type == VESC_STATUS && id != VESC_LEFT_ID && id != VESC_RIGHT_ID && id != unknownId)
    //     {
    //         unknownId = id;
    //     }
    // }
    // normal operation
    if (MODE == LEAN)
    {

        MotorTargets targets = leanMode.getTargets();
        setMotorPower(targets.left, targets.right);
    }
    else if (MODE == REMOTE)
    {
        MotorTargets targets = remoteMode.getTargets();
        setMotorPower(targets.left, targets.right);
    }

    // remote_loop() rate-limits motor updates, not this outer loop. Keep USB
    // telemetry bounded in BOTH modes so ezButton can be polled frequently.
    static uint32_t lastTelemetryMs = 0;
    const uint32_t now = millis();
    if (now - lastTelemetryMs < 50)
        return; // 20 Hz telemetry
    lastTelemetryMs = now;

    // Print CSV header once
    // static bool headerPrinted = false;
    // if (!headerPrinted && Serial && Serial.availableForWrite() >= 512)
    // {
    //     Serial.println("time_ms,F,B,L,R,total,userWeight,speed,turn,leftTargetRPM,rightTargetRPM,leftSmoothedRPM,rightSmoothedRPM,stopped,mode");
    //     headerPrinted = true;
    // }

    // Print CSV row
    // const MotorCommandTelemetry &motorTelemetry = getMotorCommandTelemetry();
    // char row[256];
    // const int rowLength = snprintf(row, sizeof(row),
    //                                "%lu,%d,%d,%d,%d,%d,%d,%0.4f,%0.4f,%.4f,%.4f,%.4f,%.4f,%d,%s\n",
    //                                millis(),
    //                                f, b, l, r, total,
    //                                userWeightEstimate,
    //                                speed, turn,
    //                                motorTelemetry.leftTargetRPM, motorTelemetry.rightTargetRPM,
    //                                motorTelemetry.leftSmoothedRPM, motorTelemetry.rightSmoothedRPM,
    //                                stopped ? 1 : 0, modeName(MODE));
    // Serial.write(reinterpret_cast<const uint8_t *>(row), rowLength);
    // // HC-06 telemetry is read-only and only emitted in lean mode. It does not
    // // depend on a USB connection or change the remote command protocol.
    // if (MODE == LEAN && Serial1.availableForWrite() >= rowLength)
    // {
    //     Serial1.write(reinterpret_cast<const uint8_t *>(row), rowLength);
    // }
}
