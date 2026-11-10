#include "RemoteDriveMode.h"
#include "Arduino.h"
#include "common.h"

unsigned long RemoteDriveMode::lastPacketMillis = 0;
bool RemoteDriveMode::softStopLatched = false;
double RemoteDriveMode::requestedLeftPower = 0.0;
double RemoteDriveMode::requestedRightPower = 0.0;

uint32_t RemoteDriveMode::receivedPackets = 0;
uint32_t RemoteDriveMode::receivedDrivePackets = 0;
size_t RemoteDriveMode::lastPacketSize = 0;
uint8_t RemoteDriveMode::lastPacketType = 0;

RemoteDriveMode::RemoteDriveMode(PacketSerial &serial) : packetSerial(serial)
{
    packetSerial.setPacketHandler(
        &RemoteDriveMode::onPacketReceived);
}

void RemoteDriveMode::onPacketReceived(const uint8_t *buffer, size_t size)
{
    ++receivedPackets;
    lastPacketSize = size;
    if (buffer == nullptr || size < 1)
        return;
    lastPacketType = buffer[0];

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

        ++receivedDrivePackets;
        lastPacketMillis = millis();
        if (!softStopLatched)
        {
            requestedLeftPower = leftInt / 32767.0;
            requestedRightPower = rightInt / 32767.0;
        }
        break;
    }

    case PACKET_SOFT_STOP:
        if (size != 1)
            return;
        Serial.println("Soft stopping because of soft stop packet");
        softStopLatched = true;
        requestedLeftPower = 0.0f;
        requestedRightPower = 0.0f;
        lastPacketMillis = millis(); // needed so it doesn't think it disconnected
        break;

    case PACKET_SOFT_STOP_CLEAR:
        if (size != 1)
            return;
        Serial.println("Clearing soft stop");
        softStopLatched = false;
        lastPacketMillis = millis(); // needed so it doesn't think it disconnected
        break;
    }
}

MotorTargets RemoteDriveMode::getTargets()
{
    const bool timedOut = millis() - lastPacketMillis >= common::CONTROLLER_TIMEOUT_MS;
    telemetry.stopped = timedOut || softStopLatched;
    if (timedOut)
    {
        requestedLeftPower = 0.0;
        requestedRightPower = 0.0;
    }

    return {static_cast<float>(requestedLeftPower), static_cast<float>(requestedRightPower)};
}

void RemoteDriveMode::reset()
{
    // Discard the previous drive command while preserving any latched soft stop.
    telemetry.stopped = true;
    lastPacketMillis = 0;
    requestedLeftPower = 0.0;
    requestedRightPower = 0.0;

    receivedPackets = 0;
    receivedDrivePackets = 0;
    lastPacketSize = 0;
    lastPacketType = 0;
}
