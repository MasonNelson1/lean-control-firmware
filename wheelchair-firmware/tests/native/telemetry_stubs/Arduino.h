#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

struct TestSerial
{
    std::string output;
    int capacity = 512;
    int availableForWrite() const { return capacity; }
    size_t write(const uint8_t *data, size_t length)
    {
        output.append(reinterpret_cast<const char *>(data), length);
        return length;
    }
};
extern TestSerial Serial, Serial1;
