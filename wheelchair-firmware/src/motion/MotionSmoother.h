#pragma once

#include <math.h>

#include "../drive_constants.h"
#include "../rpm_setpoint_pid.h"

// Turns a requested chair motion into the two wheel-speed commands.
//
// The chair's motion is smoothed on two separate axes:
//   linear = (left + right) / 2   forward speed, through the tuned RPMSetpointPID
//   turn   = (left - right) / 2   rotation, through its own rate limit
//
// The old code smoothed each wheel on its own. While the chair was speeding
// up, both wheels sat on the same acceleration limit, so the difference
// between them (the steering) disappeared until the acceleration ended, and
// came back late. Smoothing the two axes separately keeps steering alive at
// all times and lets rotation stop quickly when the rider straightens up.
//
// Pure logic: no Arduino calls, so tests/native/motion_test.cpp can run it.
class MotionSmoother
{
public:
  struct Wheels
  {
    float left;
    float right;
  };

  explicit MotionSmoother(const RPMSetpointPID::Tuning &tuning) : linear_(tuning) {}

  bool setTuning(const RPMSetpointPID::Tuning &tuning) { return linear_.setTuning(tuning); }

  /**
   * Advance by one control tick.
   *
   * @param linearTargetRPM wanted forward wheel speed (mean of both wheels)
   * @param turnTargetRPM   wanted half-difference, positive turns right
   *                        (left wheel faster)
   * @param maxSpeedMS      speed-knob limit; no wheel is commanded above it
   * @param turnAccelRPMS, turnDecelRPMS  rate limits on the half-difference,
   *                        in wheel RPM per second, for building and for
   *                        shedding rotation
   * @return wheel RPM commands, both within the speed limit
   */
  Wheels update(float linearTargetRPM, float turnTargetRPM, float dt, float maxSpeedMS,
                float turnAccelRPMS, float turnDecelRPMS)
  {
    if (!isfinite(linearTargetRPM) || !isfinite(turnTargetRPM) || !isfinite(dt) || dt <= 0.0f ||
        !isfinite(turnAccelRPMS) || !isfinite(turnDecelRPMS) || turnAccelRPMS <= 0.0f ||
        turnDecelRPMS <= 0.0f)
    {
      reset();
      return {0.0f, 0.0f};
    }
    const float linear = linear_.smoothRPM(linearTargetRPM, dt, maxSpeedMS, DriveConstants::MAX_ACCEL);
    const float maxRPM = fminf(maxSpeedMS, DriveConstants::MAX_SPEED) * DriveConstants::LINEAR_TO_RPM;
    if (!(maxRPM > 0.0f))
    {
      reset();
      return {0.0f, 0.0f};
    }

    // Shedding rotation (moving toward zero) may be quicker than building it.
    const float delta = clip(turnTargetRPM, -maxRPM, maxRPM) - turn_;
    const bool towardZero = (turn_ > 0.0f && delta < 0.0f) || (turn_ < 0.0f && delta > 0.0f);
    const float step = (towardZero ? turnDecelRPMS : turnAccelRPMS) * dt;
    turn_ += clip(delta, -step, step);

    // When the faster wheel would pass the speed limit, slow the whole chair
    // rather than clipping that one wheel: clipping one wheel changes the
    // turn the rider asked for.
    float left = linear + turn_, right = linear - turn_;
    const float fastest = fabsf(left) > fabsf(right) ? left : right;
    if (fabsf(fastest) > maxRPM)
    {
      const float shift = fastest - copysignf(maxRPM, fastest);
      left -= shift;
      right -= shift;
    }
    return {clip(left, -maxRPM, maxRPM), clip(right, -maxRPM, maxRPM)};
  }

  /** Forget all motion: both axes at rest. */
  void reset()
  {
    linear_.reset();
    turn_ = 0.0f;
  }

  /** Start from wheels that are already turning (no jump in the command). */
  void seed(float leftRPM, float rightRPM)
  {
    if (!isfinite(leftRPM) || !isfinite(rightRPM))
    {
      reset();
      return;
    }
    linear_.seed((leftRPM + rightRPM) / 2.0f);
    turn_ = (leftRPM - rightRPM) / 2.0f;
  }

  float linearRPM() const { return linear_.rpm(); }
  float turnRPM() const { return turn_; }

private:
  static float clip(float value, float low, float high) { return fminf(high, fmaxf(low, value)); }

  RPMSetpointPID linear_;
  float turn_ = 0.0f;
};
