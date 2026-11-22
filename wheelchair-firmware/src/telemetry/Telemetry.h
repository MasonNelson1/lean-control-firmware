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
        // True when a line of this length would go out over Bluetooth now.
        static bool hasRoom(size_t length);

        // Bluetooth link accounting since boot (for DBG SYS). minFree is the
        // lowest TX buffer space seen since the last takeMinFree().
        struct LinkStats
        {
            uint32_t bytes = 0, lines = 0, droppedBytes = 0, droppedLines = 0;
            int minFree = 1 << 30;
        };
        static const LinkStats &link() { return link_; }
        static int takeMinFree();

    private:
        uint32_t lastSampleMs_ = 0;
        bool headerPrinted_ = false;
        static void write(const char *data, size_t length);
        static LinkStats link_;
    };
}
