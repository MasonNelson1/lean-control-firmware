#pragma once

#include <stddef.h>
#include <stdint.h>

// Receiver side of the ESP32 remote protocol (docs/PROTOCOL.md). The chair does
// what the controller says: drive packets set left/right power (-1..+1), a
// SOFT_STOP latches zero and a CLEAR releases it.
//
// No drive packet within the timeout means zero output. It resumes when drive
// packets return. Silence also releases the stop latch, as a dropped link did on
// the Uno receiver: a controller still stopped repeats SOFT_STOP at 100 Hz.
// Malformed packets refresh nothing. No hardware access, so it can be host tested.
class RemoteLink {
 public:
  static constexpr uint8_t PACKET_DRIVE = 0x01;
  static constexpr uint8_t PACKET_SOFT_STOP = 0x02;
  static constexpr uint8_t PACKET_SOFT_STOP_CLEAR = 0x03;

  explicit RemoteLink(uint32_t timeoutMs) : timeoutMs_(timeoutMs) {}

  // nowMs must be read at or before the next service() call's nowMs.
  void onPacket(const uint8_t *buffer, size_t size, uint32_t nowMs) {
    if (buffer == nullptr || size < 1) {
      rejected_++;
      return;
    }
    switch (buffer[0]) {
      case PACKET_DRIVE:
        handleDrive(buffer, size, nowMs);
        return;
      case PACKET_SOFT_STOP:
        if (size != 1) {
          rejected_++;
          return;
        }
        lastPacketMs_ = nowMs;
        softStopped_ = true;
        clearCommand();
        return;
      case PACKET_SOFT_STOP_CLEAR:
        if (size != 1) {
          rejected_++;
          return;
        }
        lastPacketMs_ = nowMs;
        lastDriveMs_ = nowMs;
        softStopped_ = false;
        commandFresh_ = true;  // zero until the next drive packet
        return;
      default:
        rejected_++;
        return;
    }
  }

  // Call every loop, with a time read after packets were processed.
  void service(uint32_t nowMs) {
    if (commandFresh_ && nowMs - lastDriveMs_ > timeoutMs_) {
      clearCommand();
      linkTimeouts_++;
    }
    if (softStopped_ && nowMs - lastPacketMs_ > timeoutMs_) {
      softStopped_ = false;
    }
  }

  bool softStopped() const { return softStopped_; }
  bool driveAllowed() const { return !softStopped_ && commandFresh_; }
  float left() const { return driveAllowed() ? left_ : 0.0f; }
  float right() const { return driveAllowed() ? right_ : 0.0f; }

  uint32_t drivePackets() const { return drivePackets_; }
  uint32_t rejected() const { return rejected_; }
  uint32_t linkTimeouts() const { return linkTimeouts_; }
  uint32_t maxDriveGapMs() const { return maxDriveGapMs_; }

 private:
  static int16_t readLE16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
  }

  void handleDrive(const uint8_t *buffer, size_t size, uint32_t nowMs) {
    if (size != 5) {
      rejected_++;
      return;
    }
    const int16_t leftRaw = readLE16(buffer + 1);
    const int16_t rightRaw = readLE16(buffer + 3);
    if (leftRaw == INT16_MIN || rightRaw == INT16_MIN) {  // invalid per protocol
      rejected_++;
      return;
    }
    drivePackets_++;
    lastPacketMs_ = nowMs;
    if (softStopped_) return;  // while stopped, a drive packet changes nothing

    if (commandFresh_ && nowMs - lastDriveMs_ > maxDriveGapMs_) {
      maxDriveGapMs_ = nowMs - lastDriveMs_;
    }
    commandFresh_ = true;
    lastDriveMs_ = nowMs;
    left_ = leftRaw / 32767.0f;
    right_ = rightRaw / 32767.0f;
  }

  void clearCommand() {
    commandFresh_ = false;
    left_ = 0.0f;
    right_ = 0.0f;
  }

  const uint32_t timeoutMs_;
  bool softStopped_ = false;
  bool commandFresh_ = false;  // no output until a command arrives
  float left_ = 0.0f;
  float right_ = 0.0f;
  uint32_t lastDriveMs_ = 0;
  uint32_t lastPacketMs_ = 0;
  uint32_t drivePackets_ = 0;
  uint32_t rejected_ = 0;
  uint32_t linkTimeouts_ = 0;
  uint32_t maxDriveGapMs_ = 0;
};
