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
    float motorAmps = 0.0f; // the larger motor current reported, as a magnitude
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
    rampIneffective_ = false;
    checking_ = false;
    if (fmaxf(fabsf(left0_), fabsf(right0_)) < DriveConstants::STOP_HANDOFF_RPM)
      enterBrake(now);
    else
      phase_ = Phase::Ramp;
  }

  /** Start a stop when the wheel speeds are unknown: brake current only. */
  void beginBlind(uint32_t now) { begin(0.0f, 0.0f, now); }

  void cancel() { phase_ = Phase::Idle; }
  bool active() const { return phase_ != Phase::Idle; }
  // This stop went to brake current early because the wheels did not slow
  // under the speed commands. Stays set until the next begin().
  bool rampIneffective() const { return rampIneffective_; }
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
      // Are the wheels slowing at all? (See STOP_RAMP_CHECK_MS.)
      if (phase_ == Phase::Ramp && feedback.fresh)
      {
        if (!checking_)
        {
          checking_ = true;
          checkAt_ = now;
          checkSpeed_ = measured;
        }
        else if (now - checkAt_ >= STOP_RAMP_CHECK_MS)
        {
          const float asked = STOP_DECEL * LINEAR_TO_RPM * (now - checkAt_) / 1000.0f;
          // Not slowing, and the motors are not working at it either. (Wheels
          // that are not slowing while the motors brake hard, as on a steep
          // downhill, are left with the speed loop: it is doing all it can.)
          if (measured > peak * scale_ + STOP_RAMP_LAG_RPM && checkSpeed_ - measured < 0.2f * asked &&
              feedback.motorAmps < STOP_RAMP_IDLE_AMPS)
          {
            rampIneffective_ = true;
            enterBrake(now);
          }
          checkAt_ = now;
          checkSpeed_ = measured;
        }
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
      // With feedback, brake until the wheels have stopped; blind, for a fixed time.
      const uint32_t limit = feedback.fresh ? STOP_BRAKE_LIMIT_MS : STOP_BRAKE_MAX_MS;
      if (settled || now - brakeStartedAt_ >= limit)
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
  bool rampEnded_ = false, settling_ = false, rampIneffective_ = false, checking_ = false;
  uint32_t rampEndedAt_ = 0, brakeStartedAt_ = 0, settledSince_ = 0, checkAt_ = 0;
  float checkSpeed_ = 0.0f;
};
