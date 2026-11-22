#pragma once
#include <cstdint>
#include <cmath>
#include "../config/DriveSettings.h"
#include "../drive_constants.h"

// Push assist for one wheel: after the rider pushes the rim, the motor adds
// current in the same direction for a short time, so each push carries further.
//
// The boost used to depend on wheel speed alone, all the time. Boost raises
// speed and speed raised boost, so a small nudge ran the chair up to the speed
// limit by itself, and slowing the chair by hand was fought by the motor.
//
// Now a boost has to be earned by a push, and a push only counts while the
// motor is giving no boost:
//   - With the boost off, the wheel must speed up at PushAccelMS2 or more, and
//     then stop speeding up within PushMaxMs. The motor is not helping then,
//     so that is the rider. Speeding up that goes on and on is a slope, not a
//     push, however steep: it never earns a boost.
//   - The end of the push starts one boost, which fades to nothing over
//     BoostMs. Its size still follows the team's curve (assist response, max
//     boost) and on/off pulsing, and it still drops to the slowdown fraction
//     if the wheel slows.
//   - Nothing more happens until the boost is over and a new push is seen.
// So the motor can lengthen each push but can never keep the chair going, or
// start it, on its own.
class PushAssist
{
public:
    // A push starts when the wheel gains speed at 1 m/s^2 or more (3.2 wheel
    // RPM in a tenth of a second) and ends when that falls under 0.3 m/s^2.
    static constexpr float PushAccelMS2 = 1.0f;
    static constexpr float PushEndAccelMS2 = 0.3f;
    static constexpr uint32_t PushWindowMs = 100;
    static constexpr uint32_t PushMaxMs = 1000; // longer than this is not a push
    static constexpr uint32_t SampleMs = 20;    // VESC speed frames arrive at 50 Hz
    static constexpr uint32_t BoostMs = 700;    // one boost, fading out
    // After a boost, wait this long before watching for the next push, so the
    // speed the VESC reports has caught up with what the boost did.
    static constexpr uint32_t RearmMs = 100;
    // Boost is full up to 85% of the speed-knob limit and zero from 100%.
    static constexpr float speedCapFadeFraction = 0.15f;

    // Boost current (A, signed like the wheel speed) for this tick.
    float update(float currRpm, uint32_t now, float maxRPM)
    {
        if (!std::isfinite(currRpm) || !std::isfinite(maxRPM) || maxRPM <= 0.0f)
        {
            reset();
            return 0.0f;
        }
        const bool slowing = updateSlowdown(currRpm, now);
        lastMS = now;
        initialized = true;

        if (boosting)
        {
            const uint32_t elapsed = now - boostStartedAt;
            // Over, or the wheel has stopped or turned round: this push is spent.
            if (elapsed >= BoostMs || currRpm * direction < minRpm)
            {
                boosting = false;
                samples = 0; // what follows must be seen unboosted from the start
                push = Push::None;
                rearmAt = now + RearmMs;
                waitingToRearm = true;
                return 0.0f;
            }
            // The team's pulsing: on for one control tick, off for the next
            // (5 ms on, 5 ms off at the default control period). Counting
            // ticks keeps it at half the time whatever the control period is.
            pulseOn = !pulseOn;
            if (!pulseOn)
                return 0.0f;
            const float fade = 1.0f - static_cast<float>(elapsed) / BoostMs;
            const float scale = slowing ? settings::assistSlowdownBoostFraction.get() : 1.0f;
            return direction * fade * scale * calcBoost(currRpm, maxRPM);
        }

        // No boost: watch for a push.
        if (waitingToRearm)
        {
            if (static_cast<int32_t>(now - rearmAt) < 0)
                return 0.0f;
            waitingToRearm = false;
        }
        if (samples != 0 && now - lastSampleMs < SampleMs)
            return 0.0f;
        lastSampleMs = now;
        for (uint8_t i = History - 1; i > 0; --i)
            history[i] = history[i - 1];
        history[0] = currRpm;
        if (samples < History)
            ++samples;
        if (samples < History)
            return 0.0f;

        // Speed gained over the window, in the direction the wheel now turns.
        const float before = history[History - 1];
        const float gained = currRpm >= 0.0f ? currRpm - before : before - currRpm;
        const float perWindow = DriveConstants::LINEAR_TO_RPM * PushWindowMs / 1000.0f;
        if (push == Push::None)
        {
            if (std::fabs(currRpm) >= minRpm && gained >= PushAccelMS2 * perWindow)
            {
                push = Push::Rising;
                pushStartedAt = now;
            }
        }
        else if (gained < PushEndAccelMS2 * perWindow)
        {
            // The speeding-up has ended. If it was short it was a push.
            const bool wasPush = push == Push::Rising;
            push = Push::None;
            if (wasPush && std::fabs(currRpm) >= minRpm)
            {
                boosting = true;
                pulseOn = false;
                boostStartedAt = now;
                direction = currRpm > 0.0f ? 1.0f : -1.0f;
            }
        }
        else if (now - pushStartedAt > PushMaxMs)
        {
            push = Push::Sustained; // a slope: wait for it to end, then start over
        }
        return 0.0f;
    }

    void reset()
    {
        initialized = false;
        boosting = false;
        waitingToRearm = false;
        pulseOn = false;
        push = Push::None;
        samples = 0;
        direction = 0.0f;
        baselineRPM = 0.0f;
        lastMS = 0;
        lastSampleMs = 0;
        boostStartedAt = 0;
    }

    bool boostActive() const { return boosting; }

private:
    // Enough unboosted samples to span the push window.
    static constexpr uint8_t History = PushWindowMs / SampleMs + 1;

    bool initialized = false;
    bool boosting = false;
    float direction = 0.0f;
    float baselineRPM = 0.0f;
    uint32_t lastMS = 0, lastSampleMs = 0, boostStartedAt = 0, rearmAt = 0, pushStartedAt = 0;
    bool waitingToRearm = false, pulseOn = false;
    // Rising: speeding up fast enough to be a push. Sustained: it has gone on too long to be one.
    enum class Push : uint8_t { None, Rising, Sustained } push = Push::None;
    float history[History]{};
    uint8_t samples = 0;
    float minRpm = 1.0f;

    // The team's boost size: more at higher speed, shaped by the assist curve.
    float calcBoost(float currRpm, float maxRPM)
    {
        const auto &knee = settings::assistResponse.get();
        const float responseExponent = logf(knee[1]) / logf(knee[0]);
        const float speedFraction = fminf(fabsf(currRpm) / maxRPM, 1.0f);
        const float maxBoostAmps = settings::assistMaxBoostAmps.get();
        // Fade the boost out over the last part of the speed-knob range and
        // give none at or above the limit.
        const float headroom = fminf(fmaxf((1.0f - fabsf(currRpm) / maxRPM) / speedCapFadeFraction, 0.0f), 1.0f);
        const float boostAmps = powf(speedFraction, responseExponent) * maxBoostAmps * headroom;
        if (!std::isfinite(boostAmps))
            return 0.0f;

        return fminf(boostAmps, maxBoostAmps);
    }

    // True while the wheel is slowing against its recent speed (the rider is
    // braking it by hand): the boost drops to the slowdown fraction.
    bool updateSlowdown(float currRpm, uint32_t now)
    {
        constexpr float baselineTimeConstantMs = 100.0f;
        const float speed = std::fabs(currRpm);
        if (!initialized)
        {
            baselineRPM = speed;
            return false;
        }

        const float elapsedMs = now - lastMS;
        const float alpha = elapsedMs / (baselineTimeConstantMs + elapsedMs);
        baselineRPM += alpha * (speed - baselineRPM);
        return baselineRPM - speed >= settings::assistSlowdownRPM.get();
    }
};
