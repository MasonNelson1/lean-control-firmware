#include <Arduino.h>
#include <PacketSerial.h>
#include <string.h>
#include <math.h>
#include <SoftwareSerial.h>
#define STATE_PIN 4

PacketSerial packetSerial;
SoftwareSerial hc06(2,3);//2 and 3 are the pins. rx of the reciever is in 3 and tx is 2

const uint8_t PACKET_DRIVE = 0x01;
const uint8_t PACKET_SOFT_STOP = 0x02;
const uint8_t PACKET_SOFT_STOP_CLEAR = 0x03;

//  Stop if no valid drive packet arrives
// for 250 ms. Test and tune this watchdog for the real hardware.
const unsigned long DRIVE_TIMEOUT_MS = 250;
unsigned long lastDrivePacketMillis = 0;
bool softStopLatched = false;  // Fail-safe until valid commands arrive/clear is sent.

void setMotorPower(double left, double right) {
  Serial.print("LPower: ");
  Serial.print(left, 3);

  Serial.print(", RPower: ");
  Serial.println(right, 3);
}
void requestSoftStop() {
  // TODO: Replace with a controlled deceleration / soft-stop implementation.
  Serial.println("Requesting soft stop");
  setMotorPower(0.0, 0.0); // Placeholder only; not a true ramped soft stop.
  
}

void onPacketReceived(const uint8_t* buffer, size_t size) {
  
  if (buffer == nullptr || size < 1) return;

//  Serial.print("Packet bytes: ");
//  for (size_t i = 0; i < size; i++) {
//    if (buffer[i] < 0x10) Serial.print("0");
//    Serial.print(buffer[i], HEX);
//    Serial.print(" ");
//  }
//  Serial.println();

  
  switch (buffer[0]) {

    case PACKET_DRIVE: {
      //validate size
//      Serial.p/rintln("drive packet recieved");
      constexpr size_t expectedSize = 1 + 2 * sizeof(int16_t);
      if (size != expectedSize) return;
      //unpack data
      int16_t leftInt, rightInt;
      memcpy(&leftInt, buffer + 1, sizeof(leftInt));
      memcpy(&rightInt, buffer + 1 + sizeof(leftInt), sizeof(rightInt));

      double left = leftInt / 32767.0;
      double right = rightInt / 32767.0; 

      //validate data
      if (!isfinite(left) || !isfinite(right)) return;
      if (left < -1.0 || left > 1.0 || right < -1.0 || right > 1.0) return;
      //at this point we consider the packer valid
      lastDrivePacketMillis = millis();
      //enforce the soft stop
//      Serial.prin/t("latch: ");
//      Serial.println/(softStopLatched);
      if (!softStopLatched) setMotorPower(left, right);
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

    default:
      // Unknown packet type; ignore it.
      break;
  }
}

void setup() {
  Serial.begin(115200);
  hc06.begin(115200);
  pinMode(STATE_PIN, INPUT);

  packetSerial.setStream(&hc06);
  packetSerial.setPacketHandler(onPacketReceived);
  lastDrivePacketMillis = millis();
}

void loop() {
  if (digitalRead(STATE_PIN) == HIGH) {
    packetSerial.update();

    if (millis() - lastDrivePacketMillis > DRIVE_TIMEOUT_MS) {
      if (!softStopLatched) {
        Serial.println("Soft stopping because of packet delay");
        softStopLatched = true;
        requestSoftStop();
      }
   }
  }else {
    softStopLatched = false;
    lastDrivePacketMillis = millis();//keep this updated
    Serial.println("bluetooth disconnected");
  }
  
}
