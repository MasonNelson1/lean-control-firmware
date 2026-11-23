#include "lean_layout.h"

#include <algorithm>
#include <math.h>

namespace lean
{
    std::array<float, Channels> cornerForces(const Counts &raw, const Counts &emptyPlate, const Gains &gains)
    {
        std::array<float, Channels> forces{};
        for (size_t i = 0; i < Channels; ++i)
            forces[i] = std::max(0, raw[i] - emptyPlate[i]) * gains[i];
        return forces;
    }

    CentreOfPressure centreOfPressure(const std::array<float, Channels> &forces,
                                      const std::array<Point, Channels> &positions)
    {
        CentreOfPressure cop;
        float momentX = 0, momentY = 0;
        for (size_t i = 0; i < Channels; ++i)
        {
            cop.total += forces[i];
            momentX += forces[i] * positions[i].x;
            momentY += forces[i] * positions[i].y;
        }
        cop.valid = cop.total >= MinCopTotal;
        if (cop.valid)
            cop.at = {momentX / cop.total, momentY / cop.total};
        return cop;
    }

    float axisLean(float offset, float positiveReach, float negativeReach, float deadzone)
    {
        const float reach = offset >= 0 ? positiveReach : negativeReach;
        const float beyond = fabsf(offset) - deadzone;
        // Settings bounds keep reach above the deadzone; this guards a bad caller.
        if (beyond <= 0 || reach <= deadzone)
            return 0.0f;
        return copysignf(std::min(beyond / (reach - deadzone), 1.0f), offset);
    }

    NeutralZone neutralZone(Point active, const Point *passive, float deadzone, float passiveMargin)
    {
        if (!passive)
            return {active.x, active.y, active.y, deadzone, deadzone};
        return {active.x, std::min(active.y, passive->y), std::max(active.y, passive->y), deadzone, passiveMargin};
    }

    Command cornerCommand(Point cop, const NeutralZone &zone, const Reach &reach)
    {
        // Distance beyond the span's forward or back end; inside the span is 0.
        // With no relaxed posture the span is a single point and this is just
        // cop.y − neutral.y.
        const float dy = cop.y > zone.yForward ? cop.y - zone.yForward
                         : cop.y < zone.yBack  ? cop.y - zone.yBack
                                               : 0.0f;
        return {axisLean(dy, reach.forward, reach.back, zone.yDeadzone),
                axisLean(cop.x - zone.x0, reach.right, reach.left, zone.xDeadzone)};
    }

    Box deadBox(const NeutralZone &zone)
    {
        return {zone.x0 - zone.xDeadzone, zone.x0 + zone.xDeadzone,
                zone.yBack - zone.yDeadzone, zone.yForward + zone.yDeadzone};
    }

    Box fullLeanBox(const NeutralZone &zone, const Reach &reach)
    {
        return {zone.x0 - reach.left, zone.x0 + reach.right, zone.yBack - reach.back, zone.yForward + reach.forward};
    }

    void EmptyPlateTracker::start(const Counts &raw, uint32_t now)
    {
        collecting = true;
        startedAt = now;
        samples = 1;
        low = high = raw;
        for (size_t i = 0; i < Channels; ++i)
            sum[i] = raw[i];
    }

    bool EmptyPlateTracker::update(const Counts &raw, uint32_t now, bool eligible, const Counts &configured)
    {
        if (!eligible)
        {
            collecting = false;
            return false;
        }
        if (!collecting)
        {
            start(raw, now);
            return false;
        }
        for (size_t i = 0; i < Channels; ++i)
        {
            low[i] = std::min(low[i], raw[i]);
            high[i] = std::max(high[i], raw[i]);
            sum[i] += raw[i];
            if (high[i] - low[i] > EmptySteadyCounts)
            {
                start(raw, now); // something moved: begin a fresh window here
                return false;
            }
        }
        ++samples;
        if (now - startedAt < EmptyZeroWindowMs)
            return false;
        collecting = false; // the next sample starts the next window
        Counts mean{};
        for (size_t i = 0; i < Channels; ++i)
        {
            mean[i] = static_cast<int>((sum[i] + samples / 2) / samples);
            if (mean[i] - configured[i] > EmptyMaxShift || configured[i] - mean[i] > EmptyMaxShift)
                return false;
        }
        hasAdopted = true;
        adoptedBaseline = mean;
        adoptedAgainst = configured;
        return true;
    }

    Counts EmptyPlateTracker::baseline(const Counts &configured) const
    {
        return adopted(configured) ? adoptedBaseline : configured;
    }

    // Moved verbatim from LeanDriveMode.cpp (normalizeSensorPair). The plus golden
    // test checks the result is bit-identical to commit 8712aef.
    static float normalizeSensorPair(int difference, int userWeightEstimate)
    {
        if (userWeightEstimate <= 0)
            return 0.0f;
        return std::min(std::max(static_cast<float>(difference) / userWeightEstimate, -1.0f), 1.0f);
    }

    Command plusCommand(int frontMinusBack, int rightMinusLeft, int userWeightEstimate)
    {
        return {normalizeSensorPair(frontMinusBack, userWeightEstimate),
                normalizeSensorPair(rightMinusLeft, userWeightEstimate)};
    }

    Command shapeAxes(Command command, float deadband, float speedExponent, float turnExponent)
    {
        float speed = command.speed, turn = command.turn;

        // apply deadband.
        if (fabsf(speed) < deadband)
            speed = 0.0f;
        if (fabsf(turn) < deadband)
            turn = 0.0f;

        // Shape speed and turn separately before wheel mixing and PID smoothing.
        speed = copysignf(powf(fabsf(speed), speedExponent), speed);
        turn = copysignf(powf(fabsf(turn), turnExponent), turn);
        return {speed, turn};
    }

    Command shapeCommand(Command command, float deadband, float speedExponent, float turnExponent, float reverseFlipAt)
    {
        const Command shaped = shapeAxes(command, deadband, speedExponent, turnExponent);
        const float speed = shaped.speed;
        float turn = shaped.turn;
        // Reversing: flip turn so leaning back-left still swings the chair left the way
        // it feels natural. Threshold -0.3, not 0, was picked by feel on the chair; at 0
        // turning near a standstill felt wrong. Below -0.9 (hard reverse) turning is off.
        // Deliberate tuning: don't change without testing on the chair.
        // The plus layout keeps -0.3 (default argument). The corner layout passes
        // CornerReverseFlipAt: on the court (2026-11-22) slow reversing rarely got past
        // -0.3, so back-left spun the chair the wrong way.
        // (The drive pipeline uses reverseTurn() below, centred on the same threshold.)
        if (speed < reverseFlipAt)
        {
            turn *= -1;
        }
        if (speed < -0.9f)
        {
            turn = 0;
        }
        return {speed, turn};
    }

    float reverseTurn(float speed, float turn, float travel, bool reverseLocked, float reverseFlipAt)
    {
        const float forwardTravel = fminf(1.0f, fmaxf(0.0f, (travel - 0.05f) / 0.10f));
        if (reverseLocked && speed < 0.0f)
            return turn * forwardTravel;
        // shapeCommand's step rule: -1 (flipped), 0 (cut off) or 1.
        const float rule = speed < -0.9f ? 0.0f : speed < reverseFlipAt ? -1.0f : 1.0f;
        // Rolling forward: the rule fades out toward "turn as leaned".
        return turn * (rule + (1.0f - rule) * forwardTravel);
    }

    const char *layoutName(SensorLayout layout)
    {
        return layout == SensorLayout::Corner ? "CORNER" : "PLUS";
    }

} // namespace lean
