#include "common.h"

namespace common {
VescBus can1;
VescStatus vescs[2] = {{VESC_LEFT_ID}, {VESC_RIGHT_ID}};
RPMSetpointPID leftRPMSetpointPID(RPM_PID_TUNING);
RPMSetpointPID rightRPMSetpointPID(RPM_PID_TUNING);
}
