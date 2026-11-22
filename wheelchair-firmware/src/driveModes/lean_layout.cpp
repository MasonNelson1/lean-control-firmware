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

    Command cornerCommand(Point cop, Point neutral, const Reach &reach, float deadzone)
    {
        return {axisLean(cop.y - neutral.y, reach.forward, reach.back, deadzone),
                axisLean(cop.x - neutral.x, reach.right, reach.left, deadzone)};
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

    Command shapeCommand(Command command, float deadband, float speedExponent, float turnExponent)
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
        // Reversing: flip turn so leaning back-left still swings the chair left the way
        // it feels natural. Threshold -0.3, not 0, was picked by feel on the chair; at 0
        // turning near a standstill felt wrong. Below -0.9 (hard reverse) turning is off.
        // Deliberate tuning: don't change without testing on the chair.
        // Applies to every layout: it acts on the shaped speed/turn, which mean the
        // same thing whichever sensors produced them.
        if (speed < -0.3f)
        {
            turn *= -1;
        }
        if (speed < -0.9f)
        {
            turn = 0;
        }
        return {speed, turn};
    }

    const char *layoutName(SensorLayout layout)
    {
        return layout == SensorLayout::Corner ? "CORNER" : "PLUS";
    }

} // namespace lean
