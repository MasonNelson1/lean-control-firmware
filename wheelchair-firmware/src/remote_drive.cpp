// LEAN remote drive: ESP32 controller -> Bluetooth -> HC-06 -> Teensy -> VESCs.
// Lean sensors are not read here, so there is NO occupancy gate in this program.
// Serial1 carries binary packets only; debug text goes to USB Serial.
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
constexpr uint32_t PRINT_MS = 200;

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

void printStatus(float leftRPM, float rightRPM, bool vescsOk) {
  // USB writes can stall when a host stops reading. Skip a line rather than
  // delay the control loop.
  char line[160];
  const int length = snprintf(
      line, sizeof(line),
      "%-12s cmd %+.2f %+.2f | rpm %+6.1f %+6.1f | erpm %6ld %6ld | pkts %lu bad %lu timeouts %lu maxGap %lums\n",
      stateName(vescsOk), remote.left(), remote.right(), leftRPM, rightRPM,
      (long)vescs[0].erpm, (long)vescs[1].erpm,
      (unsigned long)remote.drivePackets(), (unsigned long)remote.rejected(),
      (unsigned long)remote.linkTimeouts(), (unsigned long)remote.maxDriveGapMs());
  if (length > 0 && length < (int)sizeof(line) && Serial.availableForWrite() >= length) {
    Serial.write(line, length);
  }
}

void setup() {
  Serial.begin(115200);
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
  float leftRPM = 0.0f;
  float rightRPM = 0.0f;
  if (killed || !vescsOk || !remote.driveAllowed()) {
    // Any stop bypasses smoothing and restarts both wheels from zero.
    leftPID.reset();
    rightPID.reset();
  } else {
    const float maxRPM = REMOTE_MAX_SPEED_MS * LINEAR_TO_RPM;
    leftRPM = leftPID.smoothRPM(remote.left() * maxRPM, dt, REMOTE_MAX_SPEED_MS, REMOTE_MAX_ACCEL_MS2);
    rightRPM = rightPID.smoothRPM(remote.right() * maxRPM, dt, REMOTE_MAX_SPEED_MS, REMOTE_MAX_ACCEL_MS2);
  }

#if REMOTE_MOTORS
  vescSetWheelRPM(can1, VESC_LEFT_ID, leftRPM * DIR_LEFT);
  vescSetWheelRPM(can1, VESC_RIGHT_ID, rightRPM * DIR_RIGHT);
#endif

  updateStatusLed(killed || !vescsOk);

  static uint32_t tPrint = 0;
  if (millis() - tPrint >= PRINT_MS) {
    tPrint = millis();
    printStatus(leftRPM, rightRPM, vescsOk);
  }
}
