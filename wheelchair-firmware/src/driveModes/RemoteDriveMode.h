#pragma once

#include "DriveMode.h"
#include <cstddef>
#include <cstdint>

// Hand-controller drive: wheel targets from Bluetooth packets, each -1 to 1 as
// a share of the speed-knob limit.
class RemoteDriveMode
{
private:
    static constexpr uint8_t PACKET_DRIVE = 0x01;
    static constexpr uint8_t PACKET_SOFT_STOP = 0x02;
    static constexpr uint8_t PACKET_SOFT_STOP_CLEAR = 0x03;
    static constexpr uint32_t RecentreMs = 500;
    // The controller sends exact zeros inside its own 6% deadband.
    static constexpr float CentreBand = 0.05f;

    bool stopped_ = true;
    bool havePacket = false; // no command is trusted until a packet arrives after reset()
    // Drive packets are ignored until one arrives with the stick centred:
    // after REMOTE is selected, and after the controller has been silent for
    // RecentreMs. A stick held over through a short gap carries on; one that
    // comes back after a real outage must not start the chair.
    bool needsCentre = true;
    uint32_t lastPacketMillis = 0;
    bool softStopLatched = false;
    float requestedLeftPower = 0.0f;
    float requestedRightPower = 0.0f;

public:
    void onPacketReceived(const uint8_t *buffer, size_t size, uint32_t now);
    // Targets for this tick. Zero, with stopped() true, once the controller
    // has been silent for the timeout or its soft stop is latched.
    MotorTargets getTargets(uint32_t now);
    bool stopped() const { return stopped_; }

    void reset();
};
