#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#define E2END 4283

struct TestEEPROM {
    std::array<uint8_t, E2END + 1> bytes;
    TestEEPROM() { bytes.fill(0xff); }
    uint8_t read(size_t address) const { return bytes.at(address); }
    bool failWrites = false; // simulates a worn-out or failing EEPROM
    void update(size_t address, uint8_t value) { if (!failWrites) bytes.at(address) = value; }
};
inline TestEEPROM EEPROM;
