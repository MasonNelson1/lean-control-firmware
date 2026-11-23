// Shared helpers for the drive-controller tests: simulated seat sensors on the
// CORNER layout and a 5 ms tick.
//
// council-corner: council/firmware-9 wrote these helpers for its own plus-layout
// lean math (pair differences in ADC counts, per-sensor zeros). This build
// drives from charles/dev-setup's corner centre-of-pressure input, so the
// helpers now place a rider's centre of pressure on the plate instead. The
// council's lean units are kept so its ride scenarios read the same:
// leanBy(600, 0) is a full forward lean, leanBy(-400, 0) full back,
// leanBy(0, ±500) a full turn, and a lean under 5% of those sits inside the
// deadzone (the council's deadband).
#pragma once
#include "driveModes/LeanDriveMode.h"
#include "driveModes/lean_layout.h"
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

// The simulated rider's body (leanBy): it moves toward the posture asked for
// at BodySpeedInS, stepped by bodyStep() once per control tick. The council's
// scenarios assume leans happen at once; a body that moves more than
// lean::MaxCopStep (2.5") in one 5 ms tick is what the CoP-jump check treats as
// a sensor dropping out. 2" per tick keeps leans near-instant without that.
constexpr float BodySpeedInS = 400.0f;
inline bool bodyActive = false;
inline float bodyX = 0, bodyY = 0, bodyTotal = 0, targetX = 0, targetY = 0, targetTotal = 0;
void placeCop(float x, float y, float total);

// Raw ADC readings, by channel name (F back-left, B back-right, L front-left,
// R front-right on the corner plate).
inline void rawSensors(int f, int b, int l, int r)
{
    bodyActive = false;
    sensorInputs[common::PIN_F] = f;
    sensorInputs[common::PIN_B] = b;
    sensorInputs[common::PIN_L] = l;
    sensorInputs[common::PIN_R] = r;
}
// One raw reading by pin (a failing sensor). The body stops moving so that
// bodyStep() does not overwrite it.
inline void setSensor(int pin, int value)
{
    bodyActive = false;
    sensorInputs[pin] = value;
}
// Loads above each channel's configured empty-plate reading.
inline void loads(int f, int b, int l, int r)
{
    rawSensors(settings::emptyPlateF.get() + f, settings::emptyPlateB.get() + b,
               settings::emptyPlateL.get() + l, settings::emptyPlateR.get() + r);
}
// Nobody on the seat.
inline void emptySeat() { loads(0, 0, 0, 0); }

// A rider of `total` counts with their centre of pressure at (x, y) plate
// inches. The corners are close to a rectangle: bilinear weights on its mean
// edges, then a few corrections against the firmware's own centre-of-pressure
// math (rounding to whole counts leaves about 0.002").
inline lean::Counts copCounts(float x, float y, float total)
{
    const auto &p = lean::CornerPositions;
    const float xl = (p[lean::F].x + p[lean::L].x) / 2, xr = (p[lean::B].x + p[lean::R].x) / 2;
    const float yb = (p[lean::F].y + p[lean::B].y) / 2, yf = (p[lean::L].y + p[lean::R].y) / 2;
    const auto counts = [](float v) { return static_cast<int>(lroundf(v)); };
    float aimX = x, aimY = y;
    lean::Counts out{};
    for (int pass = 0; pass < 4; ++pass)
    {
        const float wr = (aimX - xl) / (xr - xl), wf = (aimY - yb) / (yf - yb);
        out = {{counts(total * (1 - wr) * (1 - wf)), counts(total * wr * (1 - wf)),
                counts(total * (1 - wr) * wf), counts(total * wr * wf)}};
        std::array<float, lean::Channels> forces{};
        for (size_t i = 0; i < lean::Channels; ++i)
            forces[i] = static_cast<float>(out[i]);
        const auto cop = lean::centreOfPressure(forces);
        aimX += x - cop.at.x;
        aimY += y - cop.at.y;
    }
    return out;
}
inline void placeCop(float x, float y, float total)
{
    const auto c = copCounts(x, y, total);
    loads(c[0], c[1], c[2], c[3]);
}
// The centre of pressure at once (no body motion).
inline void copAt(float x, float y, float total) { placeCop(x, y, total); }
// Move the body one control tick toward its target posture.
inline void bodyStep()
{
    if (!bodyActive)
        return;
    const float dx = targetX - bodyX, dy = targetY - bodyY, d = std::hypot(dx, dy);
    const float step = BodySpeedInS * TickS;
    if (d <= step)
        bodyX = targetX, bodyY = targetY;
    else
        bodyX += dx / d * step, bodyY += dy / d * step;
    bodyTotal = targetTotal;
    placeCop(bodyX, bodyY, bodyTotal);
    bodyActive = true; // placeCop -> loads -> rawSensors cleared it
}

// The simulated rider's neutral (upright) centre of pressure and weight.
constexpr float NeutralX = 0.0f, NeutralY = -0.6f;
constexpr float RiderLoad = 1200.0f;
// The council's default full-lean ranges (counts) and the share of a lean
// that was its deadband.
constexpr float CouncilRangeForward = 600, CouncilRangeBack = 400, CouncilRangeSide = 500;
constexpr float CouncilDeadband = 0.05f;

// Council lean units -> inches of centre-of-pressure travel from neutral, so
// that the corner input's pre-curve output equals what the council's lean
// math gave for the same lean. (lean::axisLean ramps from the deadzone edge to
// full output at `reach` from neutral.)
inline float leanInches(float counts, float range, float deadzone, float reach)
{
    const float n = std::fabs(counts) / range;
    const float inches = n < CouncilDeadband ? n / CouncilDeadband * deadzone
                                             : deadzone + (reach - deadzone) * (n - CouncilDeadband) / (1 - CouncilDeadband);
    return std::copysign(inches, counts);
}
// A lean on top of the neutral posture; `common` adds load on every corner.
inline void leanBy(int forward, int right, int common = 0)
{
    const float dz = settings::cornerDeadzone.get();
    const float dy = forward >= 0 ? leanInches(forward, CouncilRangeForward, dz, settings::reachForward.get())
                                  : leanInches(forward, CouncilRangeBack, dz, settings::reachBack.get());
    const float dx = right >= 0 ? leanInches(right, CouncilRangeSide, dz, settings::reachRight.get())
                                : leanInches(right, CouncilRangeSide, dz, settings::reachLeft.get());
    const bool moving = bodyActive;
    targetX = NeutralX + dx, targetY = NeutralY + dy, targetTotal = RiderLoad + 4.0f * common;
    if (!moving) // sitting down: the posture starts where it is asked for
    {
        bodyX = targetX, bodyY = targetY, bodyTotal = targetTotal;
        placeCop(bodyX, bodyY, bodyTotal);
    }
    bodyActive = true;
}
