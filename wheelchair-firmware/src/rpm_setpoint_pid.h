#pragma once

#include <math.h>

//author codex. 

// RPM command smoothing only: this is not measured motor-speed feedback.
// Use one instance per wheel. Call reset() on a stop/fault, and bypass this
// helper when sending an immediate zero. A requested zero is otherwise smoothed.
class RPMSetpointPID {
 public:
  struct Tuning {
    float maxRPM;
    float wheelCircumferenceM;
    float lowSpeedTauS;
    float highSpeedTauS;
    float ki;                       // 1/s^2; zero disables integral
    float kd;                       // dimensionless; D acts on command motion
    float derivativeFilterS;
    float lowSpeedAccelMS2;
    float highSpeedAccelMS2;
    float maxJerkRPMPerS2;           // rate of change of RPM/s acceleration
  };

  explicit RPMSetpointPID(const Tuning &tuning)
      : tuning_(tuning), valid_(validTuning(tuning)) {}

  /**
   * Advance the smoother by one VESC motor-setpoint update.
   *
   * Call exactly once per update to this wheel's VESC motor setpoint. This method does not
   * read a clock, schedule updates, or detect skipped/extra calls.
   *
   * @param requestedRPM Desired wheel RPM, not normalized power or electrical RPM.
   *                     Used directly for the error; only the output RPM is limited.
   * @param dt Seconds since the previous VESC setpoint update; must be finite
   *           and positive (for example, 0.005f for a 5 ms update).
   * @return Next smoothed wheel RPM. The first call advances from zero.
   * Scheduling uses this instance's own smoothed RPM; wheel states are independent.
   */
  float smoothRPM(float requestedRPM, float dt) {
    if (!valid_ || !isfinite(requestedRPM) || !isfinite(dt) || dt <= 0.0f) {
      reset();
      return 0.0f;
    }

    if (havePreviousRPM_) {
      const float rate = (rpm_ - previousRPM_) / previousDt_;
      const float alpha = dt / (tuning_.derivativeFilterS + dt);
      filteredRate_ += alpha * (rate - filteredRate_);
    }
    previousRPM_ = rpm_;
    previousDt_ = dt;
    havePreviousRPM_ = true;

    // Blend low/high-speed tuning by the magnitude of the current output RPM.
    // Smoothstep rounds the blend near zero and maximum speed.
    float fraction = clip(fabsf(rpm_) / tuning_.maxRPM, 0.0f, 1.0f);
    fraction = fraction * fraction * (3.0f - 2.0f * fraction);


    const float tau = tuning_.lowSpeedTauS +
                      fraction * (tuning_.highSpeedTauS - tuning_.lowSpeedTauS);


    const float rpmPerMeter = 60.0f / tuning_.wheelCircumferenceM;//should be pre calculated, change if constants are refactored. 
    const float lowAccel = tuning_.lowSpeedAccelMS2 * rpmPerMeter;

    const float accelLimit = lowAccel + fraction *
        (tuning_.highSpeedAccelMS2 * rpmPerMeter - lowAccel);

    const float error = requestedRPM - rpm_;

    const float requestedAccel = error / tau + tuning_.ki * integral_ -
                                 tuning_.kd * filteredRate_;
    const float boundedAccel = clip(requestedAccel, -accelLimit, accelLimit);
    const float jerkStep = tuning_.maxJerkRPMPerS2 * dt;
    acceleration_ += clip(boundedAccel - acceleration_, -jerkStep, jerkStep);

    // Conditional integration: do not wind up while rate/jerk limits prevent
    // applying the requested correction. Each instance owns its own integral.
    if (tuning_.ki > 0.0f &&
        (fabsf(requestedAccel - acceleration_) < 1.0e-5f ||
         error * (requestedAccel - acceleration_) <= 0.0f)) {
      const float integralLimit = lowAccel / tuning_.ki;
      integral_ = clip(integral_ + error * dt, -integralLimit, integralLimit);
    }

    const float nextRPM = rpm_ + acceleration_ * dt;
    if (!isfinite(nextRPM) || !isfinite(filteredRate_) || !isfinite(integral_)) {
      reset();
      return 0.0f;
    }

    //enforce speed limits. 
    rpm_ = clip(nextRPM, -tuning_.maxRPM, tuning_.maxRPM);
    if (rpm_ != nextRPM) {
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
   *  Use on a stop/fault; the caller must send the immediate zero to the VESC.
   */
  void reset() {
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

 private:
  static float clip(float value, float low, float high) {
    return fminf(high, fmaxf(low, value));
  }

  static bool validTuning(const Tuning &p) {
    return isfinite(p.maxRPM) && p.maxRPM > 0.0f &&
           isfinite(p.wheelCircumferenceM) && p.wheelCircumferenceM > 0.0f &&
           isfinite(p.lowSpeedTauS) && p.lowSpeedTauS > 0.0f &&
           isfinite(p.highSpeedTauS) && p.highSpeedTauS > 0.0f &&
           isfinite(p.ki) && p.ki >= 0.0f &&
           isfinite(p.kd) && p.kd >= 0.0f &&
           isfinite(p.derivativeFilterS) && p.derivativeFilterS >= 0.0f &&
           isfinite(p.lowSpeedAccelMS2) && p.lowSpeedAccelMS2 > 0.0f &&
           isfinite(p.highSpeedAccelMS2) && p.highSpeedAccelMS2 > 0.0f &&
           p.highSpeedAccelMS2 <= p.lowSpeedAccelMS2 &&
           isfinite(p.maxJerkRPMPerS2) && p.maxJerkRPMPerS2 > 0.0f;
  }

  const Tuning tuning_;
  const bool valid_;
  float rpm_ = 0.0f;
  float acceleration_ = 0.0f;
  float integral_ = 0.0f;
  float filteredRate_ = 0.0f;
  float previousRPM_ = 0.0f;
  float previousDt_ = 0.0f;
  bool havePreviousRPM_ = false;
};
