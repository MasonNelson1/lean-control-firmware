#pragma once

#include <math.h>
#include <stdint.h>

#include "../drive_constants.h"

// Brings the chair to rest when drive is taken away (DISARM, empty seat,
// sensor fault, silent VESC, lost remote). Before this existed those cases
// sent zero current, so the chair coasted.
//
// Two phases:
//   Ramp   Both wheel-speed commands fall to zero together at STOP_DECEL, so
//          the chair stops along the curve it was on. The command starts
//          falling on the first tick and is at the full rate after
//          STOP_ONSET_S.
//   Brake  Below STOP_HANDOFF_RPM the VESC's speed mode stops regulating
//          (its minimum-ERPM setting; see drive_constants.h), so the rest of
//          the stop uses brake current, which works down to a standstill and
//          cannot reverse.
// Then the motors are released (zero current) and the chair rolls freely.
//
// Neither phase needs VESC feedback, so the same sequence runs when a VESC has
// gone silent: the ramp starts from the last command and the brake phase runs
// for its full time.
//
// Pure logic: no Arduino calls, so tests/native/motion_test.cpp can run it.
class StopController
{
public:
  enum class Phase : uint8_t { Idle, Ramp, Brake };
  enum class Drive : uint8_t { Release, RPM, Brake };
  struct Output
  {
    Drive drive;
    float left;  // wheel RPM, or brake amps
    float right;
  };
  struct Feedback
  {
    float leftRPM;
    float rightRPM;
    bool fresh; // both VESCs reported their speed recently
  };

  /**
   * Start a stop from these wheel speeds: the commands being sent, or the
   * measured speeds when no speed command was active.
   */
  void begin(float leftRPM, float rightRPM, uint32_t now)
  {
    left0_ = isfinite(leftRPM) ? leftRPM : 0.0f;
    right0_ = isfinite(rightRPM) ? rightRPM : 0.0f;
    scale_ = 1.0f;
    elapsed_ = 0.0f;
    rampEndedAt_ = 0;
    rampEnded_ = false;
    if (fmaxf(fabsf(left0_), fabsf(right0_)) < DriveConstants::STOP_HANDOFF_RPM)
      enterBrake(now);
    else
      phase_ = Phase::Ramp;
  }

  /** Start a stop when the wheel speeds are unknown: brake current only. */
  void beginBlind(uint32_t now) { begin(0.0f, 0.0f, now); }

  void cancel() { phase_ = Phase::Idle; }
  bool active() const { return phase_ != Phase::Idle; }
  Phase phase() const { return phase_; }
  // The ramp's current wheel-speed commands, for handing back to normal driving.
  float leftRPM() const { return phase_ == Phase::Ramp ? left0_ * scale_ : 0.0f; }
  float rightRPM() const { return phase_ == Phase::Ramp ? right0_ * scale_ : 0.0f; }

  /** Advance one control tick. Returns Release once the stop is complete. */
  Output update(float dt, uint32_t now, const Feedback &feedback)
  {
    using namespace DriveConstants;
    const float measured = fmaxf(fabsf(feedback.leftRPM), fabsf(feedback.rightRPM));
    if (phase_ == Phase::Ramp)
    {
      const float peak = fmaxf(fabsf(left0_), fabsf(right0_));
      // The faster wheel slows at STOP_DECEL and the other in proportion.
      // The rate builds up over STOP_ONSET_S rather than arriving as a step.
      elapsed_ += dt;
      const float onset = fminf(elapsed_ / STOP_ONSET_S, 1.0f);
      scale_ -= STOP_DECEL * onset * LINEAR_TO_RPM * dt / peak;
      // Never command below the handoff speed: the VESC's speed loop stops there.
      const float floor = STOP_HANDOFF_RPM / peak;
      if (scale_ <= floor)
      {
        scale_ = floor;
        if (!rampEnded_)
        {
          rampEnded_ = true;
          rampEndedAt_ = now;
        }
        // Hold the handoff speed until the wheels have actually come down to
        // it (they may lag the ramp), for a limited time.
        const bool wheelsDown = !feedback.fresh || measured < STOP_HANDOFF_RPM + 10.0f;
        if (wheelsDown || now - rampEndedAt_ >= STOP_RAMP_OVERRUN_MS)
          enterBrake(now);
      }
      if (phase_ == Phase::Ramp)
        return {Drive::RPM, left0_ * scale_, right0_ * scale_};
    }
    if (phase_ == Phase::Brake)
    {
      if (feedback.fresh && measured < STOP_DONE_RPM)
      {
        if (!settling_)
        {
          settling_ = true;
          settledSince_ = now;
        }
      }
      else
      {
        settling_ = false;
      }
      const bool settled = settling_ && now - settledSince_ >= STOP_SETTLE_MS;
      if (settled || now - brakeStartedAt_ >= STOP_BRAKE_MAX_MS)
        phase_ = Phase::Idle;
      else
        return {Drive::Brake, STOP_BRAKE_AMPS, STOP_BRAKE_AMPS};
    }
    return {Drive::Release, 0.0f, 0.0f};
  }

private:
  void enterBrake(uint32_t now)
  {
    phase_ = Phase::Brake;
    brakeStartedAt_ = now;
    settling_ = false;
  }

  Phase phase_ = Phase::Idle;
  float left0_ = 0.0f, right0_ = 0.0f;
  float scale_ = 0.0f, elapsed_ = 0.0f;
  bool rampEnded_ = false, settling_ = false;
  uint32_t rampEndedAt_ = 0, brakeStartedAt_ = 0, settledSince_ = 0;
};
