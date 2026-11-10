#pragma once

struct MotorTargets
{
    float left;
    float right;
};

class DriveMode
{

public:
    virtual MotorTargets getTargets() = 0;

    virtual ~DriveMode() = default;
};