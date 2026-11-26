// Host test for the stop-band guard (does not command hardware):
// g++ -std=c++17 -Wall -Wextra -Werror -I src tests/native/stop_band_guard_test.cpp -o /tmp/stop_guard_test
#include "stop_band_guard.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace common;
using stopguard::Wheel;

static int checks = 0;
static void same(float got, float want)
{
    ++checks;
    if (std::fabs(got - want) > 1e-6f)
    {
        std::printf("expected %.3f, got %.3f\n", want, got);
        assert(false);
    }
}

// The VESC's duty-zero stop band, in wheel RPM (s_pid_min_erpm = 300 ERPM on both).
constexpr float VESC_MIN_ERPM = 300.0f;
constexpr float ERPM_PER_RPM = 14.5454f * 4.0f; // vesc_can.h VESC_ERPM_PER_WHEEL_RPM

int main()
{
    // The floor is outside the VESC stop band, and an integer cast (vescSetWheelRPM
    // takes int32) keeps it there.
    assert(static_cast<int>(STOP_GUARD_FLOOR_RPM) * ERPM_PER_RPM > 1.4f * VESC_MIN_ERPM);

    // Rolling forward (1.6 m/s) and commanded 0: floor, in the wheel's direction.
    {
        Wheel g;
        same(g.apply(0.0f, 50.0f, true), STOP_GUARD_FLOOR_RPM);
        assert(g.engaged() && g.engagements() == 1);
        // Any near-stop command, either sign, while rolling.
        same(g.apply(5.0f, 40.0f, true), STOP_GUARD_FLOOR_RPM);
        same(g.apply(-3.0f, 40.0f, true), STOP_GUARD_FLOOR_RPM);
        assert(g.engagements() == 1); // still the same engagement
    }
    // Rolling forward, commanded reverse (the 18:03 inside wheel: -5 at 63): floor.
    {
        Wheel g;
        same(g.apply(-5.0f, 63.0f, true), STOP_GUARD_FLOOR_RPM);
        same(g.apply(-40.0f, 63.0f, true), STOP_GUARD_FLOOR_RPM); // big reverse too
    }
    // Rolling, command outside the stop band and the same sign: untouched.
    {
        Wheel g;
        same(g.apply(13.0f, 63.0f, true), 13.0f); // the 50 RPM clamp: measured - 50
        same(g.apply(STOP_GUARD_FLOOR_RPM, 40.0f, true), STOP_GUARD_FLOOR_RPM);
        assert(!g.engaged());
        same(g.apply(60.0f, 40.0f, true), 60.0f); // speeding up
    }
    // Slow wheel (below release) commanded 0: 0 (the short is harmless there).
    {
        Wheel g;
        same(g.apply(0.0f, 9.0f, true), 0.0f);
        same(g.apply(0.0f, -9.0f, true), 0.0f);
        assert(!g.engaged() && g.engagements() == 0);
    }
    // Stopped wheel, small turn-in-place and reverse-from-standstill commands: unchanged.
    {
        Wheel g;
        for (float cmd : {3.0f, -3.0f, 6.0f, -6.0f, -20.0f, 20.0f})
            same(g.apply(cmd, 0.0f, true), cmd);
        // A wheel creeping at turn-in-place speed, reversed: below release, allowed.
        same(g.apply(-6.0f, 7.0f, true), -6.0f);
        assert(g.engagements() == 0);
    }
    // Hysteresis: arms above ENGAGE, disarms only below RELEASE.
    {
        Wheel g;
        const float mid = 0.5f * (STOP_GUARD_RELEASE_RPM + STOP_GUARD_ENGAGE_RPM);
        // Speeding up from rest through the band: not armed until above ENGAGE.
        same(g.apply(0.0f, mid, true), 0.0f);
        assert(!g.rolling());
        same(g.apply(0.0f, STOP_GUARD_ENGAGE_RPM + 0.5f, true), STOP_GUARD_FLOOR_RPM);
        assert(g.rolling());
        // Slowing back into the band: still armed, still the floor.
        same(g.apply(0.0f, mid, true), STOP_GUARD_FLOOR_RPM);
        same(g.apply(0.0f, STOP_GUARD_RELEASE_RPM + 0.1f, true), STOP_GUARD_FLOOR_RPM);
        // Noise around RELEASE + a little doesn't chatter.
        for (int k = 0; k < 20; ++k)
            same(g.apply(0.0f, STOP_GUARD_RELEASE_RPM + (k % 2 ? 0.2f : 2.5f), true), STOP_GUARD_FLOOR_RPM);
        assert(g.engagements() == 1);
        // Below RELEASE: lets go, 0 passes.
        same(g.apply(0.0f, STOP_GUARD_RELEASE_RPM - 0.1f, true), 0.0f);
        assert(!g.rolling() && !g.engaged());
        // Back up into the band (noise): stays released.
        same(g.apply(0.0f, mid, true), 0.0f);
        assert(g.engagements() == 1);
    }
    // Reverse driving mirrors forward.
    {
        Wheel g;
        same(g.apply(0.0f, -50.0f, true), -STOP_GUARD_FLOOR_RPM);
        same(g.apply(5.0f, -63.0f, true), -STOP_GUARD_FLOOR_RPM);  // plugging forward
        same(g.apply(40.0f, -63.0f, true), -STOP_GUARD_FLOOR_RPM); // big forward
        same(g.apply(-13.0f, -63.0f, true), -13.0f);               // outside band, same sign
        const float mid = 0.5f * (STOP_GUARD_RELEASE_RPM + STOP_GUARD_ENGAGE_RPM);
        same(g.apply(0.0f, -mid, true), -STOP_GUARD_FLOOR_RPM);
        same(g.apply(0.0f, -(STOP_GUARD_RELEASE_RPM - 0.1f), true), 0.0f);
        Wheel h;
        same(h.apply(0.0f, -mid, true), 0.0f); // not armed from rest
    }
    // Direction change of the measured speed while armed (can't happen in one tick
    // physically, but the floor must always follow the measured sign).
    {
        Wheel g;
        same(g.apply(0.0f, 30.0f, true), STOP_GUARD_FLOOR_RPM);
        same(g.apply(0.0f, -30.0f, true), -STOP_GUARD_FLOOR_RPM);
    }
    // A stale measurement never arms the guard, and disarms an armed one.
    {
        Wheel g;
        same(g.apply(0.0f, 50.0f, false), 0.0f);
        same(g.apply(0.0f, 50.0f, true), STOP_GUARD_FLOOR_RPM);
        same(g.apply(0.0f, 50.0f, false), 0.0f);
        assert(!g.engaged());
    }
    // A whole hard stop: the wheel coasts down 63 -> 0 at 2 RPM per tick while the
    // command is 0. Every command sent is either the floor (wheel above RELEASE) or
    // 0 with the wheel already under RELEASE. One engagement.
    {
        Wheel g;
        for (float w = 63.0f; w >= 0.0f; w -= 2.0f)
        {
            const float out = g.apply(0.0f, w, true);
            if (w >= STOP_GUARD_RELEASE_RPM)
                same(out, STOP_GUARD_FLOOR_RPM);
            else
                same(out, 0.0f);
        }
        assert(g.engagements() == 1);
    }
    // reset() forgets the armed state.
    {
        Wheel g;
        const float mid = 0.5f * (STOP_GUARD_RELEASE_RPM + STOP_GUARD_ENGAGE_RPM);
        same(g.apply(0.0f, 30.0f, true), STOP_GUARD_FLOOR_RPM);
        g.reset();
        assert(!g.engaged() && !g.rolling());
        same(g.apply(0.0f, mid, true), 0.0f);
    }

    // Log gate: one line per change, at most one per MinGapMs, ends on the true state.
    {
        stopguard::LogGate gate;
        const uint32_t gap = stopguard::LogGate::MinGapMs;
        assert(!gate.due(false, 0));      // starts "off": nothing to say
        assert(gate.due(true, 1000));     // on
        assert(!gate.due(true, 1005));    // unchanged
        assert(!gate.due(false, 1010));   // off too soon: held back
        assert(!gate.due(true, 1020));    // back on: nothing to report
        assert(!gate.due(false, 1050));   // off, still too soon
        assert(gate.due(false, 1000 + gap)); // reported once the gap has passed
        assert(!gate.due(false, 2000));
        assert(gate.due(true, 2000));
        checks += 9;
    }

    std::printf("stop_band_guard: %d checks passed\n", checks);
    return 0;
}
