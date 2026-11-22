// Shared helpers for the lean tests: simulated sensors and a 5 ms tick.
#pragma once
#include "driveModes/LeanDriveMode.h"
#include "config/DriveSettings.h"
#include "telemetry/Telemetry.h"
#include <Arduino.h>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

inline std::string messages;
void telemetry::Telemetry::logf(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    messages += line;
}
inline bool logged(const char *text) { return messages.find(text) != std::string::npos; }

constexpr float TickS = 0.005f;
constexpr uint32_t TickMs = 5;

// Raw ADC readings.
inline void rawSensors(int f, int b, int l, int r)
{
    sensorInputs[common::PIN_F] = f;
    sensorInputs[common::PIN_B] = b;
    sensorInputs[common::PIN_L] = l;
    sensorInputs[common::PIN_R] = r;
}
// Loads above each sensor's configured zero.
inline void loads(int f, int b, int l, int r)
{
    rawSensors(settings::zeroFront.get() + f, settings::zeroBack.get() + b,
               settings::zeroLeft.get() + l, settings::zeroRight.get() + r);
}

// One rider's neutral posture on the chair's sensors (from the logged floors).
constexpr int NeutralF = 133, NeutralB = 58, NeutralL = 29, NeutralR = 109;
// Lean deltas on top of that neutral posture.
inline void lean(int forward, int right, int common = 0)
{
    loads(NeutralF + common + (forward > 0 ? forward : 0), NeutralB + common + (forward < 0 ? -forward : 0),
          NeutralL + common + (right < 0 ? -right : 0), NeutralR + common + (right > 0 ? right : 0));
}

struct LeanRig
{
    LeanDriveMode mode;
    uint32_t now = 0;
    LeanDriveMode::Event last = LeanDriveMode::Event::None;

    LeanRig() { mode.updateResponseCurve(); }
    void sample(uint32_t ms = TickMs)
    {
        for (uint32_t t = 0; t < ms; t += TickMs)
        {
            now += TickMs;
            mode.sample(TickS);
        }
    }
    // Runs calibration ticks; returns the first event other than None.
    LeanDriveMode::Event calibrate(uint32_t ms, bool held = false, bool pressed = false)
    {
        for (uint32_t t = 0; t < ms; t += TickMs)
        {
            now += TickMs;
            mode.sample(TickS);
            const auto event = mode.updateCalibration(now, held, pressed && t == 0);
            if (event != LeanDriveMode::Event::None)
                return last = event;
        }
        return LeanDriveMode::Event::None;
    }
    // A short press, release, and the still second: neutral at the current posture.
    void captureNeutral()
    {
        mode.beginCalibration(now);
        calibrate(50, /*held=*/true);
        const auto event = calibrate(3000);
        assert(event == LeanDriveMode::Event::NeutralDone && mode.calibrated());
    }
    LeanDriveMode::Command settle(uint32_t ms = 300)
    {
        sample(ms);
        return mode.command();
    }
};
