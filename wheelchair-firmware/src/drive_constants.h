#pragma once

namespace common {
constexpr float WHEEL_CIRCUMFERENCE_M = 74.75f * 0.0254f;
constexpr float LINEAR_TO_RPM = 60.0f / WHEEL_CIRCUMFERENCE_M;
constexpr float MAX_ACCEL = 2.85f;      // m/s^2
constexpr float MAX_SPEED = 4.91744f;   // m/s (11 mph)
}

namespace DriveConstants = common;
