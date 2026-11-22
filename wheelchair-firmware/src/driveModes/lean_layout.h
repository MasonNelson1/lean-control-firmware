#pragma once

// Seat sensor layouts and the math that turns each one into a lean command.
//
// Pure functions, no hardware and no settings: LeanDriveMode feeds them ADC
// counts and configuration, and the host tests call them directly.
//
// Channel order everywhere is F, B, L, R: the ADC pins' historical names
// (common::PIN_F/B/L/R). On the plus layout the names are the directions. On
// the corner layout they are just channel names; CornerPositions says where
// each one sits.
//
// experiments/tools/replay_cop.py reads the Default* constants and
// CornerPositions from this file, so keep each one a plain literal on one line.

#include <array>
#include <cstddef>
#include <cstdint>

namespace lean
{
    // Byte values are stored in EEPROM and on the config wire. Never renumber.
    enum class SensorLayout : uint8_t
    {
        Plus = 0,   // one sensor front, back, left and right of centre (until 2026-11-22)
        Corner = 1, // one sensor at each corner of a rectangle (from 2026-11-22)
    };

    constexpr size_t Channels = 4;
    enum Channel : size_t { F = 0, B = 1, L = 2, R = 3 };
    using Counts = std::array<int, Channels>;
    using Gains = std::array<float, Channels>;

    // Plate coordinates in inches: origin at the old plus centre, x to the
    // rider's right, y forward.
    struct Point
    {
        float x, y;
    };

    // Speed and turn from -1 to 1. Positive speed is forward, positive turn is
    // to the rider's right (the left wheel speeds up).
    struct Command
    {
        float speed, turn;
    };

    // ---- Corner layout ------------------------------------------------------

    // Where each channel's sensor sits, measured on the chair 2026-11-22.
    // These are hardware geometry, not tuning, so they are compile-time: moving
    // a sensor means re-drilling the plate, and a reflash goes with that. They
    // also would not fit: the per-profile EEPROM budget (415 bytes) has room for
    // the corner tuning fields below but not eight more floats.
    constexpr std::array<Point, Channels> CornerPositions = {{
        {-5.75f, -4.1875f}, // F channel: back-left corner
        {5.3125f, -4.25f},  // B channel: back-right corner
        {-5.75f, 3.0f},     // L channel: front-left corner
        {5.375f, 2.9375f},  // R channel: front-right corner
    }};

    // Empty-plate readings on the chair 2026-11-22 (seat and plate on, nobody
    // sitting), raw counts F/B/L/R. The amplifiers alone read ≈372; the rest is
    // the plate's preload on each sensor.
    constexpr int DefaultEmptyPlateF = 391;
    constexpr int DefaultEmptyPlateB = 406;
    constexpr int DefaultEmptyPlateL = 427;
    constexpr int DefaultEmptyPlateR = 400;

    // Full lean in each direction, inches of centre-of-pressure travel from the
    // rider's active neutral. Mason's E9 holds (2026-11-22): forward +2.06,
    // back −1.89, left −1.73, right +1.61. Starting points, not tuned values;
    // a dynamic calibration (hold the button, lean all four ways) replaces them
    // for the current rider.
    constexpr float DefaultReachForward = 2.0f;
    constexpr float DefaultReachBack = 1.9f;
    constexpr float DefaultReachLeft = 1.7f;
    constexpr float DefaultReachRight = 1.6f;

    // Centre-of-pressure travel from neutral that is ignored, per axis, in
    // inches. In E9's active-neutral hold the CoP stayed within 0.15" of neutral
    // 95% of the time (worst sample 0.25"), so 0.25" keeps a still rider at zero
    // (replay_cop.py prints this). Output ramps from zero at the deadzone edge,
    // so there is no step.
    constexpr float DefaultDeadzone = 0.25f;

    // Reach bounds. The minimum stays above the deadzone maximum, so every
    // accepted setting leaves a usable range in each direction.
    constexpr float MaxDeadzone = 0.5f;
    constexpr float MinReach = 0.6f;
    constexpr float MaxReach = 6.0f;

    // Below this many counts of total load the centre of pressure is noise. A
    // seated rider loads the plate by ≈1,300–2,600 counts, and the seat gate
    // disarms long before the total gets this low; this only guards the division.
    constexpr float MinCopTotal = 50.0f;

    struct Reach
    {
        float forward, back, left, right; // all positive distances in inches
    };

    struct CentreOfPressure
    {
        Point at{0, 0};
        float total = 0; // Σ force in gain-scaled counts above the empty plate
        bool valid = false;
    };

    // Force on each channel in gain-scaled counts: (raw − emptyPlate) × gain,
    // floored at zero. A corner reading below its empty-plate baseline carries no
    // load; letting it go negative would let baseline drift push the centre of
    // pressure outside the plate.
    std::array<float, Channels> cornerForces(const Counts &raw, const Counts &emptyPlate, const Gains &gains);

    // x = Σ(fᵢ·xᵢ)/Σfᵢ, y = Σ(fᵢ·yᵢ)/Σfᵢ. Invalid when the total is below MinCopTotal.
    CentreOfPressure centreOfPressure(const std::array<float, Channels> &forces,
                                      const std::array<Point, Channels> &positions = CornerPositions);

    // One axis: zero inside the deadzone, then a straight ramp to ±1 at full
    // reach, clamped beyond it. positiveReach applies to offsets above zero.
    float axisLean(float offset, float positiveReach, float negativeReach, float deadzone);

    // Speed from forward/back offset, turn from right/left offset, both from the
    // rider's neutral centre of pressure.
    Command cornerCommand(Point cop, Point neutral, const Reach &reach, float deadzone);

    // ---- Plus layout (the original math, unchanged) ---------------------------

    // F−B and R−L after floor subtraction and per-sensor gains, each divided by
    // the rider weight estimate and clamped to ±1.
    Command plusCommand(int frontMinusBack, int rightMinusLeft, int userWeightEstimate);

    // ---- Shared by every layout -----------------------------------------------

    // Deadband, response curves and the reverse-turn flip, in that order.
    // deadband is in normalised units; pass 0 to skip it.
    Command shapeCommand(Command command, float deadband, float speedExponent, float turnExponent);

    const char *layoutName(SensorLayout layout);

} // namespace lean
