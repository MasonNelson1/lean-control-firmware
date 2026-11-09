// Simple HC-06 receiver for Teensy 4.1. Prints commands; does not drive motors.
// HC-06 TX -> pin 0 (RX1), HC-06 RX -> pin 1 (TX1), STATE -> pin 4.
#include <Arduino.h>
#include <PacketSerial.h>
#include <string.h>
#include "vesc_can.h"
#include "common.h"

using namespace common;
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

void remote_setup() {
  Serial1.begin(115200);
  packetSerial.setStream(&Serial1);
  packetSerial.setPacketHandler(onPacketReceived);
  vescBegin(can1);
  delay(200);
}

void remote_loop() {
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
