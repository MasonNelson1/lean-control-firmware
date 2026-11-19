#pragma once

#include "DriveMode.h"
#include <cstddef>
#include <cstdint>

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

    RemoteTelemetry telemetry{};

    unsigned long lastPacketMillis = 0;
    bool softStopLatched = false;
    double requestedLeftPower = 0.0;
    double requestedRightPower = 0.0;

public:
    void onPacketReceived(const uint8_t *buffer, size_t size);
    MotorTargets getTargets() override;
    const RemoteTelemetry &getTelemetry() const { return telemetry; }

    void reset();
};
