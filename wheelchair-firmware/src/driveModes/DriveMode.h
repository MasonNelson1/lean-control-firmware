#pragma once

// A pair of per-wheel values: a share of the speed limit (remote) or amps (assist).
struct MotorTargets
{
    float left;
    float right;
};
