#pragma once
#include <cstdint>
#include <cmath>
#include "../config/DriveSettings.h"
#include "../drive_constants.h"

// Push assist for one wheel: when the rider pushes the rim, the motor adds
// current in the same direction for a short time.
//
// The boost used to depend on wheel speed alone, all the time. Boost raises
// speed and speed raised boost, so a small nudge ran the chair up to the speed
// limit by itself, and slowing the chair by hand was fought by the motor.
//
// Now a boost has to be earned by a push, and a push only counts while the
// motor is giving no boost:
//   - With the boost off, the wheel must speed up by PushGainRPM within
//     PushWindowMs. The motor is not helping then, so that is the rider.
//   - That starts one boost, which fades to nothing over BoostMs. Its size
//     still follows the team's curve (assist response, max boost) and pulse
//     pattern, and it still drops to the slowdown fraction if the wheel slows.
//   - Nothing more happens until the boost is over and a new push is seen.
// So the motor can lengthen each push but can never keep the chair going, or
// start it, on its own.
class PushAssist
{
public:
    // A push: at least 1 m/s^2 for a tenth of a second (3.2 wheel RPM gained),
    // unboosted. A wheelchair ramp at the legal limit (1 in 12) gives
    // 0.8 m/s^2 before rolling resistance, so rolling down one does not count.
    static constexpr float PushAccelMS2 = 1.0f;
    static constexpr uint32_t PushWindowMs = 100;
    static constexpr uint32_t SampleMs = 20; // VESC speed frames arrive at 50 Hz
    static constexpr uint32_t BoostMs = 700; // one boost, fading out
    // Boost is full up to 85% of the speed-knob limit and zero from 100%.
    static constexpr float speedCapFadeFraction = 0.15f;
    // The team's pulse pattern within a boost: 5 ms on, 5 ms off.
    static constexpr uint32_t PulseMs = 10;
    static constexpr uint32_t PulseOnMs = 5;

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
                lastSampleMs = now;
                return 0.0f;
            }
            if (elapsed % PulseMs >= PulseOnMs)
                return 0.0f; // the off half of the pulse
            const float fade = 1.0f - static_cast<float>(elapsed) / BoostMs;
            const float scale = slowing ? settings::assistSlowdownBoostFraction.get() : 1.0f;
            return direction * fade * scale * calcBoost(currRpm, maxRPM);
        }

        // No boost: watch for a push.
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

        const float before = history[History - 1];
        const float pushGain = PushAccelMS2 * DriveConstants::LINEAR_TO_RPM * PushWindowMs / 1000.0f;
        // Faster than a window ago, in the direction it is now turning.
        const float gained = currRpm >= 0.0f ? currRpm - before : before - currRpm;
        if (std::fabs(currRpm) >= minRpm && gained >= pushGain)
        {
            boosting = true;
            boostStartedAt = now;
            direction = currRpm > 0.0f ? 1.0f : -1.0f;
        }
        return 0.0f;
    }

    void reset()
    {
        initialized = false;
        boosting = false;
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
    uint32_t lastMS = 0, lastSampleMs = 0, boostStartedAt = 0;
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
