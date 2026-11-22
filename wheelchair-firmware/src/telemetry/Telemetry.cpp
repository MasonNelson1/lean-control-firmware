#include "Telemetry.h"
#include <Arduino.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace telemetry
{
    bool Telemetry::due(uint32_t now)
    {
        if (now - lastSampleMs_ < 50)
            return false;
        lastSampleMs_ = now;
        return true;
    }

    void Telemetry::write(const char *data, size_t length)
    {
        // Drop a complete line if a port is busy, instead of blocking control or
        // inserting a partial line into the shared CSV/configuration stream.
        // (USB too: with a host attached but not reading, a write can stall
        // for about 120 ms.)
        if (Serial.availableForWrite() >= static_cast<int>(length))
            Serial.write(reinterpret_cast<const uint8_t *>(data), length);
        if (Serial1.availableForWrite() >= static_cast<int>(length))
            Serial1.write(reinterpret_cast<const uint8_t *>(data), length);
    }

    void Telemetry::logf(const char *format, ...)
    {
        char line[256];
        va_list args;
        va_start(args, format);
        const int length = vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        if (length > 0 && static_cast<size_t>(length) < sizeof(line))
            write(line, static_cast<size_t>(length));
    }

    void Telemetry::publish(const TelemetryFrame &frame)
    {
        if (!headerPrinted_ && Serial.availableForWrite() >= static_cast<int>(strlen(CsvHeader)))
        {
            // Bluetooth's existing protocol is headerless CSV; keep it that way.
            Serial.write(reinterpret_cast<const uint8_t *>(CsvHeader), strlen(CsvHeader));
            headerPrinted_ = true;
        }
        char row[256];
        const size_t length = formatCSV(frame, row, sizeof(row));
        if (length)
            write(row, length);
    }
}
