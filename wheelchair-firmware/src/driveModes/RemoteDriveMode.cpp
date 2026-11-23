#include "RemoteDriveMode.h"
#include "../config/DriveSettings.h"
#include "../telemetry/Telemetry.h"

#include <cmath>
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

        const float left = leftInt / 32767.0f, right = rightInt / 32767.0f;
        if (havePacket && now - lastPacketMillis >= RecentreMs)
            needsCentre = true;
        if (needsCentre && fabsf(left) <= CentreBand && fabsf(right) <= CentreBand)
            needsCentre = false;
        lastPacketMillis = now;
        havePacket = true;
        if (!softStop_ && !needsCentre)
        {
            requestedLeftPower = left;
            requestedRightPower = right;
        }
        break;
    }

    case PACKET_SOFT_STOP:
        if (size != 1)
            return;
        telemetry::Telemetry::logf("Soft stopping because of soft stop packet\n");
        softStop_ = true;
        needsCentre = true; // clearing the soft stop must not start the chair on a held stick
        requestedLeftPower = 0.0f;
        requestedRightPower = 0.0f;
        lastPacketMillis = now; // needed so it doesn't think it disconnected
        havePacket = true;
        break;

    case PACKET_SOFT_STOP_CLEAR:
        if (size != 1)
            return;
        telemetry::Telemetry::logf("Clearing soft stop\n");
        softStop_ = false;
        lastPacketMillis = now; // needed so it doesn't think it disconnected
        havePacket = true;
        break;
    }
}

MotorTargets RemoteDriveMode::getTargets(uint32_t now)
{
    const uint32_t silence = now - lastPacketMillis;
    const bool timedOut = !havePacket || silence >= settings::controllerTimeout.get();
    // Waiting for a centred stick counts as stopped: the chair brakes rather
    // than holding a zero speed command.
    stopped_ = timedOut || softStop_ || needsCentre;
    if (timedOut)
    {
        // A stale command must not come back to life with the next packet.
        requestedLeftPower = 0.0f;
        requestedRightPower = 0.0f;
        if (havePacket && silence >= RecentreMs)
        {
            havePacket = false; // keeps the silence test safe across millis() rollover
            needsCentre = true;
        }
    }

    return {requestedLeftPower, requestedRightPower};
}

void RemoteDriveMode::reset()
{
    // Discard the previous drive command while preserving any latched soft stop.
    stopped_ = true;
    havePacket = false;
    needsCentre = true;
    requestedLeftPower = 0.0f;
    requestedRightPower = 0.0f;
}
