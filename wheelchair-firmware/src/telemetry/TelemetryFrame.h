#pragma once

#include "TelemetryTypes.h"
#include <cstddef>

namespace telemetry
{
    extern const char CsvHeader[];
    // Returns zero on overflow; never send a truncated row.
    size_t formatCSV(const TelemetryFrame &frame, char *buffer, size_t capacity);
}
