#pragma once

#include "DriveMode.h"


class DisarmDriveMode : public DriveMode
{
public:
    MotorTargets getTargets() override;
};
