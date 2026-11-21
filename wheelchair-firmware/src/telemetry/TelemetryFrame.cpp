#include "TelemetryFrame.h"
#include <cmath>
#include <cstdio>

namespace telemetry
{
    const char CsvHeader[] =
        "time_ms,F,B,L,R,total,userWeight,speed,turn,leftTargetRPM,rightTargetRPM,"
        "leftSmoothedRPM,rightSmoothedRPM,stopped,leftActualRPM,rightActualRPM,"
        "leftBoostCurrent,rightBoostCurrent,mode\n";

    float MotorFeedback::actualRPM(uint32_t now) const
    {
        return received && now - lastRpmMs <= 500 ? rpm : NAN;
    }

    size_t formatCSV(const TelemetryFrame &f, char *buffer, size_t capacity)
    {
        const bool leanActive = f.mode == common::LEAN;
        const bool rpmControl = leanActive || f.mode == common::REMOTE;
        const MotorCommands commands = rpmControl ? f.commands : MotorCommands{NAN, NAN, NAN, NAN};
        const bool stopped = f.mode == common::DISARM || (leanActive ? f.lean.stopped : f.remoteStopped);
        const int length = snprintf(buffer, capacity,
                                    "%lu,%d,%d,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%.4f,%s\n",
                                    static_cast<unsigned long>(f.timeMs),
                                    f.lean.front, f.lean.back, f.lean.left, f.lean.right,
                                    f.lean.front + f.lean.back + f.lean.left + f.lean.right,
                                    f.userWeight, f.lean.speed, f.lean.turn,
                                    commands.leftTargetRPM, commands.rightTargetRPM, commands.leftSmoothedRPM, commands.rightSmoothedRPM,
                                    stopped ? 1 : 0, f.left.actualRPM(f.timeMs), f.right.actualRPM(f.timeMs),
                                    f.boostCurrent.left, f.boostCurrent.right, common::modeName(f.mode));
        return length > 0 && static_cast<size_t>(length) < capacity ? static_cast<size_t>(length) : 0;
    }
}
