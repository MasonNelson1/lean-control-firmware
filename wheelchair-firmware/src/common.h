#pragma once

#include <Arduino.h>

#include "drive_constants.h"
#include "rpm_setpoint_pid.h"

namespace common {

enum mode { LEAN, REMOTE };

constexpr uint8_t PIN_F = A0;  // pin 14
constexpr uint8_t PIN_B = A17; // pin 41
constexpr uint8_t PIN_L = A14; // pin 38
constexpr uint8_t PIN_R = A16; // pin 40
constexpr uint8_t PIN_CAL_BUTTON = 4;
constexpr uint8_t PIN_LEAN_BUTTON = 5;
constexpr uint8_t PIN_REMOTE_BUTTON = 6;

} // namespace common
