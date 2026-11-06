#pragma once

#include <Arduino.h>

// Shared simple-drive hardware settings, hard design limits and PID tuning.
// Other firmware files can include this header without importing its names.
namespace DriveConstants {

constexpr uint8_t PIN_F = A0;    // pin 14
constexpr uint8_t PIN_B = A17;   // pin 41
constexpr uint8_t PIN_L = A14;   // pin 38
constexpr uint8_t PIN_R = A16;   // pin 40
constexpr uint8_t PIN_CAL_BUTTON = 4;
constexpr int CAL_SAMPLES = 20;//number of samples for calibration. 

constexpr float WHEEL_CIRCUMFERENCE_M = 74.75f * 0.0254f;
constexpr float LINEAR_TO_RPM = 60.0f / WHEEL_CIRCUMFERENCE_M;


// Hard design limits. Runtime knob selections may lower, never raise, these.
constexpr float MAX_ACCEL = 2.85f;      // m/s^2
constexpr float MAX_SPEED = 4.91744f;   // m/s (11 mph)
constexpr float JERK_RATE = 1.0f;       // m/s^3; existing accelToTarget helper

constexpr float RPM_PID_MAX_RPM = MAX_SPEED * 60.0f / WHEEL_CIRCUMFERENCE_M;
constexpr float RPM_PID_LOW_SPEED_TAU_S = 0.035f;// low speed rise time
constexpr float RPM_PID_HIGH_SPEED_TAU_S = 0.1f; //high speed rise time
constexpr float RPM_PID_KI = 0.0f; //integral constant
constexpr float RPM_PID_KD = 2.0f;//derivitive constant
constexpr float RPM_PID_DERIVATIVE_FILTER_S = 0.02f; //filters rpm setpoints to make derivitive response more smooth
constexpr float RPM_PID_LOW_SPEED_ACCEL_MS2 = MAX_ACCEL;
constexpr float RPM_PID_HIGH_SPEED_ACCEL_MS2 = MAX_ACCEL;
constexpr float RPM_PID_MAX_JERK_RPM_PER_S2 = 400.0f;//lower is more smooth

constexpr uint8_t VESC_LEFT_ID = 26;
constexpr uint8_t VESC_RIGHT_ID = 2;
constexpr float DIR_LEFT = +1.0f;
constexpr float DIR_RIGHT = +1.0f;

constexpr int DEFAULT_FLOOR = 400;
constexpr float REAR_GAIN = 4.0f;
constexpr float FRONT_GAIN = 1.0f;
constexpr float LEFT_GAIN = 1.0f;
constexpr float RIGHT_GAIN = 1.0f;
constexpr int CALIBRATE_BUTTON_DEBOUNCE = 50;  // ms
constexpr float DEADBAND = 0.05f;//normalized values so this is a percentage.
constexpr uint32_t LOOP_RATE = 5;             // ms (200 Hz)
constexpr float dt = LOOP_RATE / 1000.0f;             // s 


}  // namespace DriveConstants
