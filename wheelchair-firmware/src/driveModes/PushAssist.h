#pragma once
#include <cstdint>
#include <cmath>
#include "../config/DriveSettings.h"

struct AssistCurrents
{
    float left;
    float right;
};

class PushAssist
{

private:
    bool initialized = false;
    bool boosting = false;
    bool cooling = false;
    float filteredRPM = 0, direction = 0;
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
        const float boostAmps = powf(speedFraction, responseExponent) * maxBoostAmps;
        if (!std::isfinite(boostAmps))
            return 0.0f;

        return fminf(boostAmps, maxBoostAmps);
    }

public:
    static constexpr uint32_t pulseWidth = 20; // ms
    static constexpr float duty = 0.5f;
    static constexpr uint32_t boostMs = duty * pulseWidth;
    static constexpr uint32_t coolMs = pulseWidth - boostMs;
    static constexpr float maxBoostAmps = 30.0f;

    AssistCurrents getAssistCurrents(float leftRPM, float rightRPM, uint32_t now, float maxRPM);
    float update(float currRpm, uint32_t now, float maxRPM)
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
