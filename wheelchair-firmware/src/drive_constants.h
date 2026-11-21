#pragma once

namespace common {
constexpr float WHEEL_CIRCUMFERENCE_M = 74.75f * 0.0254f;
// Shared derived value, refreshed when the dashboard changes wheel circumference.
inline float LINEAR_TO_RPM = 60.0f / WHEEL_CIRCUMFERENCE_M;
constexpr float MAX_ACCEL = 2.85f * 1.3f;      // m/s^2
constexpr float MAX_SPEED = 4.91744f;   // m/s (x% of 11 mph for testing).

}

namespace DriveConstants = common;
