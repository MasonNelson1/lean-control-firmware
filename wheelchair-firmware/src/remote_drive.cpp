// LEAN remote drive: ESP32 controller -> Bluetooth -> HC-06 -> Teensy -> VESCs.
// Seat sensors are read for telemetry only and never affect control, so there is
// NO occupancy gate in this program.
// Serial1 carries binary packets only; telemetry and status go to USB Serial.
#include <Arduino.h>
#include <PacketSerial.h>
#include "drive_constants.h"
#include "remote_link.h"
#include "rpm_setpoint_pid.h"
#include "vesc_can.h"

using namespace DriveConstants;

// 0 = compute and print only, no CAN at all. 1 = command the VESCs.
#define REMOTE_MOTORS 1

// Bench limits: full stick with the controller knob at 100%.
constexpr float REMOTE_MAX_SPEED_MS = 1.0f;
constexpr float REMOTE_MAX_ACCEL_MS2 = 1.0f;
static_assert(REMOTE_MAX_SPEED_MS <= MAX_SPEED, "Remote speed exceeds the design limit");
static_assert(REMOTE_MAX_ACCEL_MS2 <= MAX_ACCEL, "Remote accel exceeds the design limit");

constexpr uint32_t REMOTE_TIMEOUT_MS = 250;      // no drive packet this long = zero output
constexpr uint32_t VESC_STATUS_TIMEOUT_MS = 200;
constexpr uint32_t BLUETOOTH_BAUD = 115200;      // must match the HC-06's baud setting
constexpr int TELEMETRY_USER_WEIGHT = 0;         // no calibration here, so not measured

auto &bluetooth = Serial1;  // HC-06: pin 0 = RX1, pin 1 = TX1
uint8_t bluetoothRxBuffer[256];
PacketSerial packetSerial;
RemoteLink remote(REMOTE_TIMEOUT_MS);

const RPMSetpointPID::Tuning RPM_PID_TUNING = {
    RPM_PID_LOW_SPEED_TAU_S, RPM_PID_HIGH_SPEED_TAU_S,
    RPM_PID_KI, RPM_PID_KD, RPM_PID_DERIVATIVE_FILTER_S,
    RPM_PID_LOW_SPEED_ACCEL_MS2, RPM_PID_HIGH_SPEED_ACCEL_MS2,
    RPM_PID_MAX_JERK_RPM_PER_S2};
RPMSetpointPID leftPID(RPM_PID_TUNING);
RPMSetpointPID rightPID(RPM_PID_TUNING);

VescBus can1;
VescStatus vescs[2] = {{VESC_LEFT_ID}, {VESC_RIGHT_ID}};
bool killed = false;  // USB 's': latched until 'g'; the remote cannot clear it

void onRemotePacket(const uint8_t *buffer, size_t size) {
  remote.onPacket(buffer, size, millis());
}

bool vescsReporting() {
#if REMOTE_MOTORS
  return vescFresh(vescs[0], VESC_STATUS_TIMEOUT_MS) &&
         vescFresh(vescs[1], VESC_STATUS_TIMEOUT_MS);
#else
  return true;
#endif
}

const char *stateName(bool vescsOk) {
  if (killed) return "KILLED (g)";
  if (!vescsOk) return "NO VESC";
  if (remote.softStopped()) return "SOFT STOP";
  if (!remote.driveAllowed()) return "NO LINK";
  return "DRIVING";
}

// Off = stopped, solid = driving, 5 Hz = kill or VESC fault.
void updateStatusLed(bool fault) {
  const bool on = fault ? (millis() / 100) % 2 : remote.driveAllowed();
  digitalWrite(LED_BUILTIN, on);
}

// USB writes can stall when a host stops reading. Skip a line rather than delay
// the control loop.
void writeIfRoom(const char *line, int length, int capacity) {
  if (length > 0 && length < capacity && Serial.availableForWrite() >= length) {
    Serial.write(line, length);
  }
}

// One status line each time the state changes, e.g. when the link drops.
void printStateChange(bool vescsOk) {
  static const char *lastState = nullptr;
  const char *state = stateName(vescsOk);
  if (state == lastState) return;
  lastState = state;

  char line[100];
  const int length = snprintf(
      line, sizeof(line), "** %s (pkts %lu bad %lu timeouts %lu maxGap %lums) **\n", state,
      (unsigned long)remote.drivePackets(), (unsigned long)remote.rejected(),
      (unsigned long)remote.linkTimeouts(), (unsigned long)remote.maxDriveGapMs());
  writeIfRoom(line, length, sizeof(line));
}

int aboveDefaultFloor(uint8_t pin, float gain) {
  const int raw = analogRead(pin);
  return raw > DEFAULT_FLOOR ? (int)((raw - DEFAULT_FLOOR) * gain) : 0;
}

// Same columns as simple_drive.cpp, so the debug viewer reads both programs.
// speed and turn come from the controller's commands, not from a lean.
void printTelemetry(float leftTargetRPM, float rightTargetRPM, float leftRPM, float rightRPM,
                    float speed, float turn, bool stopped) {
  static bool headerPrinted = false;
  if (!headerPrinted) {
    headerPrinted = true;
    Serial.println("time_ms,F,B,L,R,total,userWeight,speed,turn,leftTargetRPM,rightTargetRPM, leftSmoothedRPM, rightSmoothedRPM, ,stopped");
  }

  const int f = aboveDefaultFloor(PIN_F, FRONT_GAIN);
  const int b = aboveDefaultFloor(PIN_B, REAR_GAIN);
  const int l = aboveDefaultFloor(PIN_L, LEFT_GAIN);
  const int r = aboveDefaultFloor(PIN_R, RIGHT_GAIN);
  const int total = f + b + l + r;

  char line[160];
  const int length = snprintf(
      line, sizeof(line),
      "%lu,%d,%d,%d,%d,%d,%d,%0.4f,%0.4f,%.4f,%.4f,%.4f,%.4f,%d\n",
      millis(),
      f, b, l, r, total,
      TELEMETRY_USER_WEIGHT,
      speed, turn,
      leftTargetRPM, rightTargetRPM, leftRPM, rightRPM,
      stopped ? 1 : 0);
  writeIfRoom(line, length, sizeof(line));
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogReadAveraging(4);
  pinMode(LED_BUILTIN, OUTPUT);
  bluetooth.begin(BLUETOOTH_BAUD);
  bluetooth.addMemoryForRead(bluetoothRxBuffer, sizeof(bluetoothRxBuffer));
  packetSerial.setStream(&bluetooth);
  packetSerial.setPacketHandler(&onRemotePacket);
#if REMOTE_MOTORS
  vescBegin(can1);
#endif
  Serial.printf("LEAN REMOTE DRIVE: motors %s, left VESC %d, right VESC %d, max %.2f m/s\n",
                REMOTE_MOTORS ? "LIVE" : "OFF (print only)", VESC_LEFT_ID, VESC_RIGHT_ID,
                REMOTE_MAX_SPEED_MS);
  Serial.println("  Follows the controller. s = kill, g = clear kill");
}

void loop() {
  packetSerial.update();
  // Read the time after update() so a packet's timestamp is never newer than it.
  remote.service(millis());

#if REMOTE_MOTORS
  CAN_message_t msg;
  while (can1.read(msg)) vescHandleFrame(msg, vescs, 2);
#endif

  while (Serial.available()) {
    const char c = Serial.read();
    if (c == 's' || c == 'S') killed = true;
    if (c == 'g' || c == 'G') killed = false;
  }

  static uint32_t tSend = 0;
  if (millis() - tSend < LOOP_RATE) return;
  tSend = millis();

  const bool vescsOk = vescsReporting();
  const bool stopped = killed || !vescsOk || !remote.driveAllowed();
  float leftTargetRPM = 0.0f;
  float rightTargetRPM = 0.0f;
  float leftRPM = 0.0f;
  float rightRPM = 0.0f;
  float speed = 0.0f;
  float turn = 0.0f;
  if (stopped) {
    // Any stop bypasses smoothing and restarts both wheels from zero.
    leftPID.reset();
    rightPID.reset();
  } else {
    const float maxRPM = REMOTE_MAX_SPEED_MS * LINEAR_TO_RPM;
    speed = (remote.left() + remote.right()) / 2.0f;
    turn = (remote.left() - remote.right()) / 2.0f;
    leftTargetRPM = remote.left() * maxRPM;
    rightTargetRPM = remote.right() * maxRPM;
    leftRPM = leftPID.smoothRPM(leftTargetRPM, dt, REMOTE_MAX_SPEED_MS, REMOTE_MAX_ACCEL_MS2);
    rightRPM = rightPID.smoothRPM(rightTargetRPM, dt, REMOTE_MAX_SPEED_MS, REMOTE_MAX_ACCEL_MS2);
  }

#if REMOTE_MOTORS
  vescSetWheelRPM(can1, VESC_LEFT_ID, leftRPM * DIR_LEFT);
  vescSetWheelRPM(can1, VESC_RIGHT_ID, rightRPM * DIR_RIGHT);
#endif

  updateStatusLed(killed || !vescsOk);

  printStateChange(vescsOk);
  //printTelemetry(leftTargetRPM, rightTargetRPM, leftRPM, rightRPM, speed, turn, stopped);
}
