#pragma once
#include <cstdint>
#include <cmath>
#include "../config/DriveSettings.h"

class PushAssist
{
public:
    struct Tuning
    {
        float maxBoostAmps;
        float slowdownThresholdRPM;
        float slowdownBoostFraction;
        float kneeInput;
        float kneeOutput;
    };

private:
    bool initialized = false;
    bool boosting = false;
    bool cooling = false;
    float filteredRPM = 0, direction = 0;
    float baselineRPM = 0.0f;
    uint32_t lastMS = 0, pulseMS = 0, cooldownMs = 0;
    float amps = 0.0f;
    float minRpm = 1.0f;

    float calcBoost(float currRpm, float maxRPM, const Tuning &tuning)
    {
        if (!std::isfinite(currRpm) || !std::isfinite(maxRPM) || maxRPM <= 0.0f)
            return 0.0f;

        const float responseExponent = logf(tuning.kneeOutput) / logf(tuning.kneeInput);
        const float speedFraction = fminf(fabsf(currRpm) / maxRPM, 1.0f);
        const float maxBoostAmps = tuning.maxBoostAmps;
        const float boostAmps = powf(speedFraction, responseExponent) * maxBoostAmps;
        if (!std::isfinite(boostAmps))
            return 0.0f;

        return fminf(boostAmps, maxBoostAmps);
    }

    bool updateSlowdown(float currRpm, uint32_t now, float thresholdRPM)
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
        return baselineRPM - speed >= thresholdRPM;
    }

    float updateBoost(float currRpm, uint32_t now, float maxRPM, const Tuning &tuning)
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
                return direction * calcBoost(currRpm, maxRPM, tuning);
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
            return direction * calcBoost(currRpm, maxRPM, tuning);
        }
        return 0.0f;
    }

public:
    static constexpr uint32_t pulseWidth = 10; // ms
    static constexpr float duty = 0.5f;
    static constexpr uint32_t boostMs = duty * pulseWidth;
    static constexpr uint32_t coolMs = pulseWidth - boostMs;

    float update(float currRpm, uint32_t now, float maxRPM)
    {
        const auto &knee = settings::assistResponse.get();
        return update(currRpm, now, maxRPM,
                      {settings::assistMaxBoostAmps.get(), settings::assistSlowdownRPM.get(),
                       settings::assistSlowdownBoostFraction.get(), knee[0], knee[1]});
    }

    float update(float currRpm, uint32_t now, float maxRPM, const Tuning &tuning)
    {
        const float scale = updateSlowdown(currRpm, now, tuning.slowdownThresholdRPM)
                                ? tuning.slowdownBoostFraction : 1.0f;
        return scale * updateBoost(currRpm, now, maxRPM, tuning);
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
