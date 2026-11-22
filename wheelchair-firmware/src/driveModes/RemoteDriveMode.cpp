#include "RemoteDriveMode.h"
#include "../config/DriveSettings.h"
#include "../telemetry/Telemetry.h"

#include <cstring>

void RemoteDriveMode::onPacketReceived(const uint8_t *buffer, size_t size, uint32_t now)
{
    if (buffer == nullptr || size < 1)
        return;

    switch (buffer[0])
    {
    case PACKET_DRIVE:
    {
        if (size != 1 + 2 * sizeof(int16_t))
            return;

        int16_t leftInt, rightInt;
        memcpy(&leftInt, buffer + 1, sizeof(leftInt));
        memcpy(&rightInt, buffer + 1 + sizeof(leftInt), sizeof(rightInt));
        if (leftInt == INT16_MIN || rightInt == INT16_MIN)
            return;

        lastPacketMillis = now;
        havePacket = true;
        if (!softStopLatched)
        {
            requestedLeftPower = leftInt / 32767.0f;
            requestedRightPower = rightInt / 32767.0f;
        }
        break;
    }

    case PACKET_SOFT_STOP:
        if (size != 1)
            return;
        telemetry::Telemetry::logf("Soft stopping because of soft stop packet\n");
        softStopLatched = true;
        requestedLeftPower = 0.0f;
        requestedRightPower = 0.0f;
        lastPacketMillis = now; // needed so it doesn't think it disconnected
        havePacket = true;
        break;

    case PACKET_SOFT_STOP_CLEAR:
        if (size != 1)
            return;
        telemetry::Telemetry::logf("Clearing soft stop\n");
        softStopLatched = false;
        lastPacketMillis = now; // needed so it doesn't think it disconnected
        havePacket = true;
        break;
    }
}

MotorTargets RemoteDriveMode::getTargets(uint32_t now)
{
    const bool timedOut = !havePacket || now - lastPacketMillis >= settings::controllerTimeout.get();
    stopped_ = timedOut || softStopLatched;
    if (timedOut)
    {
        // A stale command must not come back to life with the next packet's timestamp.
        havePacket = false;
        requestedLeftPower = 0.0f;
        requestedRightPower = 0.0f;
    }

    return {requestedLeftPower, requestedRightPower};
}

void RemoteDriveMode::reset()
{
    // Discard the previous drive command while preserving any latched soft stop.
    stopped_ = true;
    havePacket = false;
    requestedLeftPower = 0.0f;
    requestedRightPower = 0.0f;
}
