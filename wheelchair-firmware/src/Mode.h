#pragma once

namespace common
{
enum mode { LEAN, REMOTE, DISARM, ASSIST };

inline const char *modeName(mode value)
{
    switch (value)
    {
    case LEAN: return "LEAN";
    case REMOTE: return "REMOTE";
    case DISARM: return "DISARM";
    case ASSIST: return "ASSIST";
    default: return "UNKNOWN";
    }
}
}
