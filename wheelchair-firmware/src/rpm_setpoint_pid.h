#pragma once

#include <math.h>
#include "drive_constants.h"

// author codex.

// RPM command smoothing only: this is not measured motor-speed feedback.
// Use one instance per wheel. Call reset() on a stop/fault, and bypass this
// helper when sending an immediate zero. A requested zero is otherwise smoothed.
class RPMSetpointPID
{
public:
  struct Tuning
  {
    float lowSpeedTauS;
    float highSpeedTauS;
    float ki; // 1/s^2; zero disables integral
    float kd; // dimensionless; D acts on command motion
    float derivativeFilterS;
    float lowSpeedAccelMS2;
    float highSpeedAccelMS2;
    float maxJerkRPMPerS2; // rate of change of RPM/s acceleration
  };

  static constexpr Tuning defaultTuning()
  {
    return {0.03f, 0.1f, 0.0f, 2.0f, 0.02f,
            DriveConstants::MAX_ACCEL, DriveConstants::MAX_ACCEL, 500.0f};
  }

  explicit RPMSetpointPID(const Tuning &tuning)
      : tuning_(tuning), valid_(validTuning(tuning)) {}

  /**
   * Update PID tuning without resetting the smoother.
   *
   * Preserves RPM, acceleration, integral, and derivative history.
   * Invalid tuning is rejected, leaving the existing tuning unchanged.
   *
   *
   * @return true if the new tuning was accepted.
   */
  bool setTuning(const Tuning &newTuning)
  {
    if (!validTuning(newTuning))
    {
      return false;
    }

    tuning_ = newTuning;
    valid_ = true;

    return true;
  }


  /**
   * Advance the smoother by one VESC motor-setpoint update.
   *
   * Call exactly once per update to this wheel's VESC motor setpoint. This method does not
   * read a clock, schedule updates, or detect skipped/extra calls.
   *
   * @param requestedRPM Desired wheel RPM
   *                     .
   * @param dt Seconds since the previous VESC setpoint update; must be finite
   *           and positive (for example, 0.005f for a 5 ms update).
   * @param maxSpeedMS User speed limit in m/s, capped at the shared firmware limit.
   * @param maxAccelMS2 User wheel acceleration limit in m/s^2, capped at the
   *                    shared 2.85 m/s^2 design limit and the tuned limits.
   *                    It limits speeding up only (|RPM| increasing). Braking
   *                    toward zero always uses the tuned/design limits.
   * A zero limit returns zero and clears state. Negative/nonfinite inputs also
   * reset to zero. Lowered limits take precedence over jerk continuity, including
   * immediate clipping when the speed limit drops below the current output.
   * @return Next smoothed wheel RPM. The first call advances from zero.
   * Scheduling uses this instance's own smoothed RPM; wheel states are independent.
   */
  float smoothRPM(float requestedRPM, float dt, float maxSpeedMS, float maxAccelMS2)
  {
    if (!valid_ || !isfinite(requestedRPM) || !isfinite(dt) || dt <= 0.0f ||
        !isfinite(maxSpeedMS) || !isfinite(maxAccelMS2) ||
        maxSpeedMS <= 0.0f || maxAccelMS2 <= 0.0f)
    {
      reset();
      return 0.0f;
    }

    const float speedLimitMS = fminf(maxSpeedMS, DriveConstants::MAX_SPEED);
    const float maxRPM = speedLimitMS * DriveConstants::LINEAR_TO_RPM;
    const float maxAccel = fminf(maxAccelMS2, DriveConstants::MAX_ACCEL);

    if (havePreviousRPM_)
    {
      const float rate = (rpm_ - previousRPM_) / previousDt_;
      const float alpha = dt / (tuning_.derivativeFilterS + dt);
      filteredRate_ += alpha * (rate - filteredRate_);
    }
    previousRPM_ = rpm_;
    previousDt_ = dt;
    havePreviousRPM_ = true;

    // Blend low/high-speed tuning by the magnitude of the current output RPM.
    // Smoothstep rounds the blend near zero and maximum speed.
    float fraction = clip(fabsf(rpm_) / maxRPM, 0.0f, 1.0f);
    fraction = fraction * fraction * (3.0f - 2.0f * fraction);

    const float tau = tuning_.lowSpeedTauS +
                      fraction * (tuning_.highSpeedTauS - tuning_.lowSpeedTauS);

    const float lowAccel = fminf(tuning_.lowSpeedAccelMS2, maxAccel) *
                           DriveConstants::LINEAR_TO_RPM;
    const float highAccel = fminf(tuning_.highSpeedAccelMS2, maxAccel) *
                            DriveConstants::LINEAR_TO_RPM;
    // Braking (reducing |RPM|) is never limited by the user acceleration
    // selection: it always gets the tuned limits capped only by the design cap.
    const float lowBrake = fminf(tuning_.lowSpeedAccelMS2, DriveConstants::MAX_ACCEL) *
                           DriveConstants::LINEAR_TO_RPM;
    const float highBrake = fminf(tuning_.highSpeedAccelMS2, DriveConstants::MAX_ACCEL) *
                            DriveConstants::LINEAR_TO_RPM;

    const float driveLimit = lowAccel + fraction * (highAccel - lowAccel);
    const float brakeLimit = lowBrake + fraction * (highBrake - lowBrake);
    // Positive acceleration speeds up a forward (or stopped) wheel and brakes a
    // reversing one; negative acceleration is the mirror image.
    const float upperLimit = rpm_ < 0.0f ? brakeLimit : driveLimit;
    const float lowerLimit = rpm_ > 0.0f ? -brakeLimit : -driveLimit;

    const float error = requestedRPM - rpm_;

    const float requestedAccel = error / tau + tuning_.ki * integral_ -
                                 tuning_.kd * filteredRate_;
    const float boundedAccel = clip(requestedAccel, lowerLimit, upperLimit);
    const float jerkStep = tuning_.maxJerkRPMPerS2 * dt;
    acceleration_ += clip(boundedAccel - acceleration_, -jerkStep, jerkStep);
    // A lower user-selected acceleration cap applies on this update, even if
    // reaching it exceeds the normal jerk limit. Increasing the cap does not
    // force acceleration upward; normal PID/jerk control still governs that.
    // The user cap only restricts speeding up; braking keeps the design cap.
    const float userDriveLimit = maxAccel * DriveConstants::LINEAR_TO_RPM;
    const float designBrakeLimit = DriveConstants::MAX_ACCEL * DriveConstants::LINEAR_TO_RPM;
    acceleration_ = clip(acceleration_,
                         rpm_ > 0.0f ? -designBrakeLimit : -userDriveLimit,
                         rpm_ < 0.0f ? designBrakeLimit : userDriveLimit);

    // Conditional integration: do not wind up while rate/jerk limits prevent
    // applying the requested correction. Each instance owns its own integral.
    if (tuning_.ki > 0.0f &&
        (fabsf(requestedAccel - acceleration_) < 1.0e-5f ||
         error * (requestedAccel - acceleration_) <= 0.0f))
    {
      const float integralLimit = lowAccel / tuning_.ki;
      integral_ = clip(integral_ + error * dt, -integralLimit, integralLimit);
    }

    const float nextRPM = rpm_ + acceleration_ * dt;
    if (!isfinite(nextRPM) || !isfinite(filteredRate_) || !isfinite(integral_))
    {
      reset();
      return 0.0f;
    }

    // enforce speed limits.
    rpm_ = clip(nextRPM, -maxRPM, maxRPM);
    if (rpm_ != nextRPM)
    {
      // A hard speed boundary takes precedence over jerk continuity. Ordinary
      // target crossings may still overshoot within these absolute bounds.
      acceleration_ = 0.0f;
      integral_ = 0.0f;
      filteredRate_ = 0.0f;
      havePreviousRPM_ = false;
    }
    return rpm_;
  }

  /** Clear command RPM, acceleration, integral and derivative history to zero.
   *
   */
  void reset()
  {
    rpm_ = 0.0f;
    acceleration_ = 0.0f;
    integral_ = 0.0f;
    filteredRate_ = 0.0f;
    previousRPM_ = 0.0f;
    previousDt_ = 0.0f;
    havePreviousRPM_ = false;
  }

  /** Return whether the constructor's tuning values are valid. */
  bool isValid() const { return valid_; }

  // Read-only internals for telemetry (DBG M). rpm() is this smoother's own
  // output before main.cpp clamps it to the measured RPM ± MAX_RPM_COMMAND_DIFF.
  float rpm() const { return rpm_; }
  float acceleration() const { return acceleration_; } // RPM/s
  float integral() const { return integral_; }         // RPM·s
  float filteredRate() const { return filteredRate_; } // RPM/s, the D term's input

private:
  static float clip(float value, float low, float high)
  {
    return fminf(high, fmaxf(low, value));
  }

  static bool validTuning(const Tuning &p)
  {
    return isfinite(p.lowSpeedTauS) && p.lowSpeedTauS > 0.0f &&
           isfinite(p.highSpeedTauS) && p.highSpeedTauS > 0.0f &&
           isfinite(p.ki) && p.ki >= 0.0f &&
           isfinite(p.kd) && p.kd >= 0.0f &&
           isfinite(p.derivativeFilterS) && p.derivativeFilterS >= 0.0f &&
           isfinite(p.lowSpeedAccelMS2) && p.lowSpeedAccelMS2 > 0.0f &&
           isfinite(p.highSpeedAccelMS2) && p.highSpeedAccelMS2 > 0.0f &&
           p.highSpeedAccelMS2 <= p.lowSpeedAccelMS2 &&
           isfinite(p.maxJerkRPMPerS2) && p.maxJerkRPMPerS2 > 0.0f;
  }

  Tuning tuning_;
  bool valid_;
  float rpm_ = 0.0f;
  float acceleration_ = 0.0f;
  float integral_ = 0.0f;
  float filteredRate_ = 0.0f;
  float previousRPM_ = 0.0f;
  float previousDt_ = 0.0f;
  bool havePreviousRPM_ = false;
};
