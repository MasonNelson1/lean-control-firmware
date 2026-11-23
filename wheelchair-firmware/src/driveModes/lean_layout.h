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
    //
    // Plate-on check (2026-11-22): 28.2 lb set at four known spots, gains 1.0,
    // located within 0.4" (typically 0.25"):
    //   true (0, −2.19) → (−0.15, −1.95)    true (−2.375, 0) → (−2.43, +0.19)
    //   true (0, +2.25) → (+0.40, +2.37)    true (+2.125, 0) → (+2.38, +0.31)
    // debugViewer/plate-view.js keeps a copy for drawing; a dashboard test
    // checks the two match.
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
    // rider's neutral zone (see NeutralZone). Mason's E9 holds (2026-11-22),
    // from active neutral: forward +2.06, back −1.89, left −1.73, right +1.61.
    // With a relaxed posture captured, forward is measured from it instead
    // (E9: +1.32 from passive). Starting points, not tuned values; a dynamic
    // calibration (hold the button, lean all four ways) replaces them for the
    // current rider.
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

    // Margin around the active-to-relaxed span on the forward/back axis, used
    // instead of the deadzone once a relaxed posture is captured. In E9 a still
    // rider's CoP stayed within 0.13–0.15" of either posture 95% of the time.
    constexpr float DefaultPassiveMargin = 0.15f;

    // A second calibrate press within this long of the neutral press records the
    // relaxed (passive) posture instead of re-taking neutral.
    constexpr uint32_t PassiveWindowMs = 10000;
    // A "relaxed" posture further than this from neutral, either way, is a lean,
    // not a slump, and is rejected. E9 passive sat 0.74" forward of active; E7
    // (Charles, feet on footrest) ≈0.6".
    constexpr float MaxPassiveOffset = 1.5f;

    // Reach bounds. The minimum stays above the deadzone and margin maximum, so
    // every accepted setting leaves a usable range in each direction.
    constexpr float MaxDeadzone = 0.5f;
    // 1.2": a lazy hold calibration must not turn a slump into full speed (E9's
    // passive slump was 0.74" forward of active). E9's real leans were 1.3–2.1".
    constexpr float MinReach = 1.2f;
    constexpr float MaxReach = 6.0f;
    // Hold calibration: full output at this fraction of the rider's peak lean, so full
    // speed/turn doesn't need a maximal lean (court test 2026-11-22).
    constexpr float ReachUseFraction = 0.85f;

    // Neutral calibration on the corner layout is rejected unless every channel
    // carries at least this load: a dead or unplugged sensor reads ≈372, below
    // its empty-plate baseline, so it shows up as ~0. In E9's active neutral the
    // lightest corner carried 230 counts (13% of 1,725). On the court (2026-11-22)
    // Mason sitting back put only 72-77 of ~2,050 counts on front-left, and the old
    // 50-count-and-5% rule rejected every press. A dead channel reads <= 0 here, so
    // an absolute floor of 30 counts is enough; the share rule is gone.
    constexpr float MinNeutralChannelLoad = 30.0f;  // counts above the empty plate

    // Largest believable centre-of-pressure move between two control ticks
    // (2–15 ms apart), in inches. Healthy E9 riding peaked at 0.54" per 50 ms
    // (11.7 in/s); getting out of the seat at 1.17" per 50 ms (24 in/s), i.e.
    // ≤0.36" per tick even at a 15 ms period. A channel that drops out moves the
    // CoP at once: in E9's sweep F by ≥1.96", B by ≥1.07", R by ≥0.53" (1.06" in
    // 95% of postures) and L by ≥0.22" (1.13" median). A lightly loaded channel
    // dying is hard to see and, for that moment, does little harm.
    // Raised 0.75 -> 2.5 on 2026-11-22 after it misfired during normal riding on the
    // court (16:50:02, all four sensors healthy). Real riding in LEAN reached 1.75"
    // per 20 Hz sample (p99.9 1.02"). At 2.5" it only catches gross glitches, not a
    // single dead channel (those step the CoP 0.22-1.96"); that needs a better detector.
    constexpr float MaxCopStep = 2.5f;
    // Corner layout: reverse turning flips as soon as the chair is clearly reversing.
    // Was -0.3 (plus tuning); on the court slow reversing stayed above it. Charles, 2026-11-22.
    constexpr float CornerReverseFlipAt = -0.1f;

    // Below this many counts of total load the centre of pressure is noise. A
    // seated rider loads the plate by ≈1,300–2,600 counts, and the seat gate
    // disarms long before the total gets this low; this only guards the division.
    constexpr float MinCopTotal = 50.0f;

    // Automatic empty-seat zero: while disarmed with nobody seated, a window of
    // this long with every channel steady adopts the window's mean as the
    // empty-plate baseline (RAM only). E9/E10 empty plate: σ ≈ 1–2 counts, and a
    // 3 s window typically spans 5–12 counts per channel.
    constexpr uint32_t EmptyZeroWindowMs = 3000;
    constexpr int EmptySteadyCounts = 12; // max − min per channel within the window
    // Refuse a zero this far from the configured baseline on any channel: that is
    // something resting on the seat (or the plate off), not drift. Re-seating the
    // plate moved readings ≤10 counts (391/406/427/400 → 383/396/420/404).
    constexpr int EmptyMaxShift = 30;

    // The seat must be empty this long before a hold-calibrated reach is dropped
    // as belonging to the previous rider.
    constexpr uint32_t ForgetReachEmptyMs = 3000;

    // Corner each channel sits at, for messages and the dashboard.
    constexpr const char *CornerNames[Channels] = {"back-left", "back-right", "front-left", "front-right"};
    constexpr const char ChannelLetters[Channels + 1] = "FBLR";

    struct Reach
    {
        float forward, back, left, right; // all positive distances in inches
        bool operator==(const Reach &o) const
        {
            return forward == o.forward && back == o.back && left == o.left && right == o.right;
        }
    };

    // Where the rider counts as "not leaning", in plate inches. Speed is zero
    // while the CoP's y sits in [yBack − yDeadzone, yForward + yDeadzone]; turn
    // is zero while |x − x0| ≤ xDeadzone. Forward reach is measured from
    // yForward, back reach from yBack.
    struct NeutralZone
    {
        float x0, yBack, yForward;
        float xDeadzone, yDeadzone;
    };

    // Active neutral alone: yBack = yForward = active.y and both deadzones are
    // `deadzone` (the original corner behaviour). With a relaxed posture, the
    // forward/back span runs from one posture to the other and the y deadzone
    // becomes `passiveMargin`; turning keeps the active x and `deadzone`.
    NeutralZone neutralZone(Point active, const Point *passive, float deadzone, float passiveMargin);

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

    // Speed from the CoP's forward/back distance outside the neutral zone, turn
    // from its right/left distance from x0.
    Command cornerCommand(Point cop, const NeutralZone &zone, const Reach &reach);

    // The edges of the dead box and the full-lean box, for telemetry and drawing:
    // {left x, right x, back y, front y}.
    struct Box
    {
        float left, right, back, front;
    };
    Box deadBox(const NeutralZone &zone);
    Box fullLeanBox(const NeutralZone &zone, const Reach &reach);

    // Feeds on 20 Hz samples while the chair is disarmed and nobody is seated,
    // and adopts a fresh empty-plate baseline after EmptyZeroWindowMs of steady
    // readings. The adopted baseline only applies while the configured baseline
    // it was measured against is unchanged, so a dashboard edit wins.
    class EmptyPlateTracker
    {
    public:
        // Returns true when this sample completed a window and a baseline was adopted.
        bool update(const Counts &raw, uint32_t now, bool eligible, const Counts &configured);
        // The baseline to use: the adopted one, or `configured` if none applies.
        Counts baseline(const Counts &configured) const;
        bool adopted(const Counts &configured) const { return hasAdopted && adoptedAgainst == configured; }

    private:
        void start(const Counts &raw, uint32_t now);
        bool collecting = false;
        uint32_t startedAt = 0;
        int samples = 0;
        Counts low{}, high{};
        std::array<long, Channels> sum{};
        bool hasAdopted = false;
        Counts adoptedBaseline{}, adoptedAgainst{};
    };

    // ---- Plus layout (the original math, unchanged) ---------------------------

    // F−B and R−L after floor subtraction and per-sensor gains, each divided by
    // the rider weight estimate and clamped to ±1.
    Command plusCommand(int frontMinusBack, int rightMinusLeft, int userWeightEstimate);

    // ---- Shared by every layout -----------------------------------------------

    // Deadband, response curves and the reverse-turn flip, in that order.
    // deadband is in normalised units; pass 0 to skip it.
    Command shapeCommand(Command command, float deadband, float speedExponent, float turnExponent, float reverseFlipAt = -0.3f);

    const char *layoutName(SensorLayout layout);

} // namespace lean
