#pragma once
#include <cstdint>
#include <cmath>
#include "../config/DriveSettings.h"

class PushAssist
{

private:
    bool initialized = false;
    bool boosting = false;
    bool cooling = false;
    float filteredRPM = 0, direction = 0;
    float baselineRPM = 0.0f;
    uint32_t lastMS = 0, pulseMS = 0, cooldownMs = 0;
    float amps = 0.0f;
    float minRpm = 1.0f;

    float calcBoost(float currRpm, float maxRPM)
    {
        if (!std::isfinite(currRpm) || !std::isfinite(maxRPM) || maxRPM <= 0.0f)
            return 0.0f;

        const auto &knee = settings::assistResponse.get();
        const float responseExponent = logf(knee[1]) / logf(knee[0]);
        const float speedFraction = fminf(fabsf(currRpm) / maxRPM, 1.0f);
        const float maxBoostAmps = settings::assistMaxBoostAmps.get();
        // The boost pushes the way the wheel already turns, so left alone it
        // feeds on itself. Fade it out over the last part of the speed-knob
        // range and give none at or above the limit.
        const float headroom = fminf(fmaxf((1.0f - fabsf(currRpm) / maxRPM) / speedCapFadeFraction, 0.0f), 1.0f);
        const float boostAmps = powf(speedFraction, responseExponent) * maxBoostAmps * headroom;
        if (!std::isfinite(boostAmps))
            return 0.0f;

        return fminf(boostAmps, maxBoostAmps);
    }

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

    float updateBoost(float currRpm, uint32_t now, float maxRPM)
    {
        if (!initialized)
        {
            initialized = true;
            boosting = true;
            filteredRPM = currRpm;
            lastMS = now;
            return 0.0f;
        }

        const uint32_t elapsed = now - lastMS;
        lastMS = now;
        filteredRPM += (float(elapsed) / (60.0f + elapsed)) * (currRpm - filteredRPM);
        direction = filteredRPM > 0 ? 1.0f : -1.0f;
        if (boosting)
        {
            if (now - pulseMS < boostMs)
            {
                return direction * calcBoost(currRpm, maxRPM);
            }
            boosting = false;
            cooling = true;
            cooldownMs = now;
        }

        if (cooling)
        {
            if (now - cooldownMs < coolMs)
            {
                return 0.0f;
            }
            cooling = false;
            boosting = true;
            pulseMS = now;
            return direction * calcBoost(currRpm, maxRPM); // we are exiting cooling, so return something as we enter boost.
        }
        return 0.0f;
    }

public:
    // Boost is full up to 85% of the speed-knob limit and zero from 100%.
    static constexpr float speedCapFadeFraction = 0.15f;
    static constexpr uint32_t pulseWidth = 10; // ms
    static constexpr float duty = 0.5f;
    static constexpr uint32_t boostMs = duty * pulseWidth;
    static constexpr uint32_t coolMs = pulseWidth - boostMs;

    float update(float currRpm, uint32_t now, float maxRPM)
    {
        const float scale = updateSlowdown(currRpm, now) ? settings::assistSlowdownBoostFraction.get() : 1.0f;
        return scale * updateBoost(currRpm, now, maxRPM);
    }

    void reset()
    {
        initialized = false;
        boosting = false;
        cooling = false;
        filteredRPM = 0.0f;
        direction = 0.0f;
        lastMS = 0;
        pulseMS = 0;
        cooldownMs = 0;
        amps = 0.0f;
    }
};
