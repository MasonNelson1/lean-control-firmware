#pragma once

#include "DriveMode.h"
#include <PacketSerial.h>

struct RemoteTelemetry
{
    bool stopped = true;
};

class RemoteDriveMode : public DriveMode
{
private:
    static constexpr uint8_t PACKET_DRIVE = 0x01;
    static constexpr uint8_t PACKET_SOFT_STOP = 0x02;
    static constexpr uint8_t PACKET_SOFT_STOP_CLEAR = 0x03;

    PacketSerial &packetSerial;
    RemoteTelemetry telemetry{};

    // Shared state for the static callback
    static unsigned long lastPacketMillis;
    static bool softStopLatched;
    static double requestedLeftPower;
    static double requestedRightPower;

    static uint32_t receivedPackets;
    static uint32_t receivedDrivePackets;
    static size_t lastPacketSize;
    static uint8_t lastPacketType;

    static void onPacketReceived(
        const uint8_t *buffer,
        size_t size);

public:
    explicit RemoteDriveMode(PacketSerial &serial);

    MotorTargets getTargets() override;
    const RemoteTelemetry &getTelemetry() const { return telemetry; }

    void reset();
};
