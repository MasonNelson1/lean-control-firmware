#pragma once

#include "TelemetryFrame.h"

namespace telemetry
{
    // Single owner of human-readable output. Binary configuration replies retain
    // their PacketSerial framing; all writes run sequentially from the main loop.
    class Telemetry
    {
    public:
        bool due(uint32_t now);
        void publish(const TelemetryFrame &frame);
        static void logf(const char *format, ...) __attribute__((format(printf, 1, 2)));

    private:
        uint32_t lastSampleMs_ = 0;
        bool headerPrinted_ = false;
        static void write(const char *data, size_t length);
    };
}
