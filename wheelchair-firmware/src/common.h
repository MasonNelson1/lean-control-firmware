#pragma once

#include <Arduino.h>

#include "drive_constants.h"
#include "rpm_setpoint_pid.h"
#include "vesc_can.h"

void remote_setup();
void remote_loop();
using namespace std;
// Shared simple-drive hardware settings, hard design limits and PID tuning.
// Other firmware files can include this header without importing its names.
namespace common {
enum mode {
    LEAN,
    REMOTE
};

constexpr uint8_t PIN_F = A0;    // pin 14
constexpr uint8_t PIN_B = A17;   // pin 41
constexpr uint8_t PIN_L = A14;   // pin 38
constexpr uint8_t PIN_R = A16;   // pin 40
constexpr uint8_t PIN_CAL_BUTTON = 4;
constexpr uint8_t PIN_LEAN_BUTTON = 5;
constexpr uint8_t PIN_REMOTE_BUTTON = 6;
constexpr int CAL_SAMPLES = 20;//number of samples for calibration. 

constexpr mode DEFAULT_MODE = LEAN;

// Hard design limits. Runtime knob selections may lower, never raise, these.
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
constexpr int BUTTON_DEBOUNCE = 50;  // ms
constexpr float DEADBAND = 0.05f;//normalized values so this is a percentage.
// Signed power response for speed and turn, applied after the deadband.
// (0.5, 0.25) gives output = sign(input) * abs(input)^2.
constexpr float INPUT_RESPONSE_KNEE_INPUT = 0.5f;
constexpr float INPUT_RESPONSE_KNEE_OUTPUT = 0.25f;
static_assert(INPUT_RESPONSE_KNEE_INPUT > 0.0f && INPUT_RESPONSE_KNEE_INPUT < 1.0f,
              "Response knee input must be between zero and one");
static_assert(INPUT_RESPONSE_KNEE_OUTPUT > 0.0f &&
              INPUT_RESPONSE_KNEE_OUTPUT <= INPUT_RESPONSE_KNEE_INPUT,
              "Response knee output must be positive and no greater than input");
constexpr uint32_t LOOP_RATE = 5;             // ms (200 Hz)
constexpr float dt = LOOP_RATE / 1000.0f;             // s 


extern VescBus can1;
extern VescStatus vescs[2];
// ---------- RPM SETPOINT PID  ----------
const RPMSetpointPID::Tuning RPM_PID_TUNING = {
    RPM_PID_LOW_SPEED_TAU_S, RPM_PID_HIGH_SPEED_TAU_S,
    RPM_PID_KI, RPM_PID_KD, RPM_PID_DERIVATIVE_FILTER_S,
    RPM_PID_LOW_SPEED_ACCEL_MS2, RPM_PID_HIGH_SPEED_ACCEL_MS2,
    RPM_PID_MAX_JERK_RPM_PER_S2};

extern RPMSetpointPID leftRPMSetpointPID;
extern RPMSetpointPID rightRPMSetpointPID;
// Convert linear acceleration (m/s^2) to angular acceleration (RPM/s)
inline float linearAccelToRPMs(float accel) {
    return accel * (60.0f / WHEEL_CIRCUMFERENCE_M);
}

// Convert angular acceleration (RPM/s) to linear acceleration (m/s^2)
inline float rpmsToLinearAccel(float rpms) {
    return rpms * (WHEEL_CIRCUMFERENCE_M / 60.0f);
}

inline float speedToRPM(float speed) {
  return linearAccelToRPMs(speed);
}


// This function takes doubles from -1 to 1 for the left and right motors and sets their speeds to those values
//where the value is the percentage of max speed. 
// PID smoothing is applied prior to using the setpoints. 
inline void setMotorPower(double left, double right) {
  float leftTargetRPM = constrain(left, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);
  float rightTargetRPM = constrain(right, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);
  float leftRpms = leftRPMSetpointPID.smoothRPM(leftTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
  float rightRpms = rightRPMSetpointPID.smoothRPM(rightTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
  vescSetWheelRPM(can1, VESC_LEFT_ID, leftRpms);
  vescSetWheelRPM(can1, VESC_RIGHT_ID, rightRpms);
}
}  // namespace common
