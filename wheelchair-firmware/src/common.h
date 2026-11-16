#pragma once

#include <Arduino.h>

#include "drive_constants.h"
#include "rpm_setpoint_pid.h"

namespace common {

enum mode { LEAN, REMOTE, DISARM};

constexpr uint8_t PIN_F = A15;  // pin 14
constexpr uint8_t PIN_B = A17; // pin 41
constexpr uint8_t PIN_L = A14; // pin 38
constexpr uint8_t PIN_R = A16; // pin 40
constexpr uint8_t PIN_CAL_BUTTON = 4;
constexpr uint8_t PIN_LEAN_BUTTON = 24;
constexpr uint8_t PIN_DISARM_BUTTON = 25;
constexpr uint8_t PIN_REMOTE_BUTTON = 26;
constexpr uint8_t PIN_LEAN_LED = 28;
constexpr uint8_t PIN_REMOTE_LED = 30;
constexpr uint8_t PIN_DISARM_LED = 29;
constexpr uint8_t PIN_CAL_LED = 27;
constexpr uint8_t PIN_CONNECTED_LED = 31;


} // namespace common
