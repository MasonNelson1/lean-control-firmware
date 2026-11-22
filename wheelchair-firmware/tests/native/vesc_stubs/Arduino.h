#pragma once
// Host stand-ins for the VESC CAN code (vesc_can.h, vesc_probe.h,
// vesc_monitor.h, vesc_apply.h) and Telemetry.cpp. A test sets fakeMillis
// and the Bluetooth TX space (Serial1.capacity).
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>

struct TestSerial
{
    std::string output;
    int capacity = 4096;
    int availableForWrite() const { return capacity; }
    size_t write(const uint8_t *data, size_t length)
    {
        output.append(reinterpret_cast<const char *>(data), length);
        return length;
    }
};
extern TestSerial Serial, Serial1;
inline uint32_t fakeMillis = 0;
inline uint32_t millis() { return fakeMillis; }
inline uint32_t micros() { return fakeMillis * 1000; }
using std::min;
using std::max;
