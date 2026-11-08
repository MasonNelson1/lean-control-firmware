// Simple HC-06 receiver for Teensy 4.1. Prints commands; does not drive motors.
// HC-06 TX -> pin 0 (RX1), HC-06 RX -> pin 1 (TX1), STATE -> pin 4.
#include <Arduino.h>
#include <PacketSerial.h>
#include <string.h>
#include "vesc_can.h"
#include "rpm_setpoint_pid.h"
#include "drive_constants.h"

using namespace DriveConstants;
constexpr uint8_t STATE_PIN = 3;
constexpr uint8_t PACKET_DRIVE = 0x01;
constexpr uint8_t PACKET_SOFT_STOP = 0x02;
constexpr uint8_t PACKET_SOFT_STOP_CLEAR = 0x03;
constexpr unsigned long DRIVE_TIMEOUT_MS = 250;

PacketSerial packetSerial;
unsigned long lastDrivePacketMillis = 0;
bool softStopLatched = false;
double requestedLeftPower = 0.0;
double requestedRightPower = 0.0;



VescBus can1;
VescStatus vescs[2] = { {VESC_LEFT_ID}, {VESC_RIGHT_ID} };

// ---------- RPM SETPOINT PID  ----------
const RPMSetpointPID::Tuning RPM_PID_TUNING = {
    RPM_PID_LOW_SPEED_TAU_S, RPM_PID_HIGH_SPEED_TAU_S,
    RPM_PID_KI, RPM_PID_KD, RPM_PID_DERIVATIVE_FILTER_S,
    RPM_PID_LOW_SPEED_ACCEL_MS2, RPM_PID_HIGH_SPEED_ACCEL_MS2,
    RPM_PID_MAX_JERK_RPM_PER_S2};

RPMSetpointPID leftRPMSetpointPID(RPM_PID_TUNING);
RPMSetpointPID rightRPMSetpointPID(RPM_PID_TUNING);


// This function takes doubles from -1 to 1 for the left and right motors and sets their speeds to those values
//where the value is the percentage of max speed. 
// PID smoothing is applied prior to using the setpoints. 
void setMotorPower(double left, double right) {
  float leftTargetRPM = constrain(left, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);
  float rightTargetRPM = constrain(right, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);
  float leftRpms = leftRPMSetpointPID.smoothRPM(leftTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
  float rightRpms = rightRPMSetpointPID.smoothRPM(rightTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
  Serial.printf("Setting motor power to: L:%.3f, R: %0.3f\n", left, right);
  vescSetWheelRPM(can1, VESC_LEFT_ID, leftRpms);
  vescSetWheelRPM(can1, VESC_RIGHT_ID, rightRpms);
}

//this should rapidly ramp down to 0, not just instantly command zero. 
//need to test if the PID controller properly does this or maybe create a separate estop pid tuning that aggressively decelerates. 
void requestSoftStop() {
  requestedLeftPower = 0.0;
  requestedRightPower = 0.0;
}

void onPacketReceived(const uint8_t *buffer, size_t size) {
  if (buffer == nullptr || size < 1) return;

  switch (buffer[0]) {
    case PACKET_DRIVE: {
      if (size != 1 + 2 * sizeof(int16_t)) return;

      int16_t leftInt, rightInt;
      memcpy(&leftInt, buffer + 1, sizeof(leftInt));
      memcpy(&rightInt, buffer + 1 + sizeof(leftInt), sizeof(rightInt));
      if (leftInt == INT16_MIN || rightInt == INT16_MIN) return;

      lastDrivePacketMillis = millis();
      if (!softStopLatched) {
        requestedLeftPower = leftInt / 32767.0;
        requestedRightPower = rightInt / 32767.0;
      }
      break;
    }

    case PACKET_SOFT_STOP:
      if (size != 1) return;
      Serial.println("Soft stopping because of soft stop packet");
      softStopLatched = true;
      requestSoftStop();
      break;

    case PACKET_SOFT_STOP_CLEAR:
      if (size != 1) return;
      Serial.println("Clearing soft stop");
      softStopLatched = false;
      lastDrivePacketMillis = millis();
      break;
  }
}

// HC-06 configuration bridge: module TX -> pin 0, module RX -> pin 1.
// Set this to the module's CURRENT UART baud before sending AT commands.
constexpr unsigned long HC06_BAUD = 115200;

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);
  // pinMode(STATE_PIN, INPUT);
  packetSerial.setStream(&Serial1);
  packetSerial.setPacketHandler(onPacketReceived);
  // lastDrivePacketMillis = millis();
  vescBegin(can1);
  delay(200);
  Serial.println("ready to recieve");
}

void loop() {
  packetSerial.update();

  //rate limit the loop. 
  //this is placed right before the motor communication and pid smoothing because pid needs to run at a fixed rate. 
  static uint32_t tSend = 0;
  if (millis() - tSend < LOOP_RATE) {
    return;
  }
  tSend = millis();
  setMotorPower(requestedLeftPower, requestedRightPower);
}
