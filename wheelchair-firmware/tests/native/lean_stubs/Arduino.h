#pragma once
#include <cstdint>
#include <algorithm>
constexpr uint8_t A0 = 14, A1 = 15, A15 = 39, A17 = 41, A14 = 38, A16 = 40;
constexpr int LOW = 0, HIGH = 1;
inline int sensorInputs[64]{};
inline int analogRead(uint8_t pin) { return sensorInputs[pin]; }
inline void digitalWrite(uint8_t, int) {}
inline void delay(unsigned long) {}
template<class T> inline T constrain(T value, T low, T high) {
    return std::min(high, std::max(low, value));
}
