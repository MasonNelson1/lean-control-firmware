#include "common.h"

namespace common {

static MotorCommandTelemetry motorCommandTelemetry = {};

const MotorCommandTelemetry &getMotorCommandTelemetry() {
  return motorCommandTelemetry;
}

RPMSetpointPID leftRPMSetpointPID(RPM_PID_TUNING);
RPMSetpointPID rightRPMSetpointPID(RPM_PID_TUNING);


//all this vesc and can stuff must be in the same file because of how can works internally. 
VescStatus vescs[2] = {{VESC_LEFT_ID}, {VESC_RIGHT_ID}};

VescBus can1;

void beginMotorCan() {
  vescBegin(can1);
}

bool readMotorCan(CAN_message_t &msg) {
  return can1.read(msg);
}

void setMotorPower(double left, double right) {
  float leftTargetRPM = constrain(left, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);
  float rightTargetRPM = constrain(right, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);
  float leftRpms = leftRPMSetpointPID.smoothRPM(leftTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
  float rightRpms = rightRPMSetpointPID.smoothRPM(rightTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
  vescSetWheelRPM(can1, VESC_LEFT_ID, leftRpms);
  vescSetWheelRPM(can1, VESC_RIGHT_ID, rightRpms);
  motorCommandTelemetry = {leftTargetRPM, rightTargetRPM, leftRpms, rightRpms};
}

//end vesc and can stuff. 
}
