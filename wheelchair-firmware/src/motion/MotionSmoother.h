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

    // The faster wheel is forward speed plus turn, and it may not pass the
    // speed limit. So a turn takes its share of the limit out of the forward
    // target, before smoothing: the chair then slows down and speeds back up
    // through the smoother, inside its acceleration and jerk limits.
    const float room = maxRPM - fabsf(turn_);
    const float linear = linear_.smoothRPM(clip(linearTargetRPM, -room, room), dt, maxSpeedMS, DriveConstants::MAX_ACCEL);
    // While the forward speed is still coming down to make that room, the
    // turn waits for it. Nothing here moves forward speed outside the smoother.
    const float turn = clip(turn_, -(maxRPM - fabsf(linear)), maxRPM - fabsf(linear));
    return {linear + turn, linear - turn};
  }

  /**
   * Call after update() when wheel feedback is fresh: keep the command on
   * each axis within maxLeadRPM of the wheels.
   *
   * Without this a held wheel (a curb, a mat edge, a helper's hands) lets
   * the command climb to the full target while the wheel stands still; when
   * the wheel comes free the command is far ahead and the VESC drives flat
   * out to catch up. Forward acceleration restarts from rest when this acts.
   */
  void limitLead(float measuredLinearRPM, float measuredTurnRPM, float maxLeadRPM)
  {
    if (!isfinite(measuredLinearRPM) || !isfinite(measuredTurnRPM) || !(maxLeadRPM >= 0.0f))
      return;
    const float linear = clip(linear_.rpm(), measuredLinearRPM - maxLeadRPM, measuredLinearRPM + maxLeadRPM);
    if (linear != linear_.rpm())
      linear_.seed(linear);
    turn_ = clip(turn_, measuredTurnRPM - maxLeadRPM, measuredTurnRPM + maxLeadRPM);
  }

  /**
   * Call before update() when wheel feedback is fresh.
   *
   * The wheels cannot always accelerate as fast as the command does, so the
   * command can run ahead of them. If the rider then asks for less than the
   * chair is actually doing, the chair would carry on speeding up (or
   * turning harder) until the command had unwound back past the wheels.
   * This drops the command to the real speed on that axis, so the chair
   * starts slowing on the next tick. The mirror case (command fell faster
   * than the wheels slowed, rider asks for more) is handled the same way.
   *
   * @param measuredLinearRPM mean of the measured wheel speeds
   * @param measuredTurnRPM   half their difference (left minus right)
   */
  void unwind(float measuredLinearRPM, float measuredTurnRPM, float linearTargetRPM, float turnTargetRPM)
  {
    if (!isfinite(measuredLinearRPM) || !isfinite(measuredTurnRPM))
      return;
    if (ranAhead(linear_.rpm(), measuredLinearRPM, linearTargetRPM, LinearUnwindRPM))
      linear_.seed(measuredLinearRPM);
    if (ranAhead(turn_, measuredTurnRPM, turnTargetRPM, TurnUnwindRPM))
      turn_ = measuredTurnRPM;
  }
  // The command must be this far from the wheels before unwind() acts, so
  // ordinary tracking error and measurement noise never trigger it.
  static constexpr float LinearUnwindRPM = 5.0f;
  static constexpr float TurnUnwindRPM = 2.0f;

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
  // The forward-speed smoother, read-only, for telemetry (DBG M).
  const RPMSetpointPID &linear() const { return linear_; }
  float turnRPM() const { return turn_; }

private:
  static float clip(float value, float low, float high) { return fminf(high, fmaxf(low, value)); }
  // The command is on one side of the measured value and the target on the other.
  static bool ranAhead(float command, float measured, float target, float margin)
  {
    return (command > measured + margin && target < measured) ||
           (command < measured - margin && target > measured);
  }

  RPMSetpointPID linear_;
  float turn_ = 0.0f;
};
