// Writes a tap-format CSV (like logs/live/current.csv) of one simulated bench
// run, driven by the real firmware routine (src/bench_test.h), so
// test_bench_report.py checks bench_report.py against the exact line formats.
//   g++ -std=c++17 -I src tools/tests/bench_emit.cpp -o bench_emit && ./bench_emit [rightDrag] [abortAtMs]
#include "bench_test.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

struct Motor
{
    float rpm = 0, current = 0, drag = 1.0f, tau = 0.12f;
    void step(const bench::Output &o, float dt)
    {
        if (o.cmd == bench::Cmd::Rpm)
        {
            rpm += (o.value - rpm) * (dt / tau);
            current = (rpm >= 0 ? 1 : -1) * (drag + 0.02f * fabsf(rpm));
        }
        else
        {
            const float dec = (o.cmd == bench::Cmd::Brake ? o.value * 40.0f : 30.0f) * dt;
            rpm = fabsf(rpm) <= dec ? 0 : rpm - (rpm > 0 ? dec : -dec);
            current = o.cmd == bench::Cmd::Brake && rpm != 0 ? -o.value : 0;
        }
    }
};

static void host(uint32_t ms)
{
    const uint32_t t = 20 * 3600 * 1000 + ms; // 20:00:00 plus firmware ms
    printf("%02u:%02u:%02u.%03u,", t / 3600000 % 24, t / 60000 % 60, t / 1000 % 60, t % 1000);
}

int main(int argc, char **argv)
{
    const float rightDrag = argc > 1 ? static_cast<float>(atof(argv[1])) : 1.0f;
    const long abortAt = argc > 2 ? atol(argv[2]) : -1;
    printf("host_time,time_ms,F,B,L,R,total,userWeight,speed,turn,leftTargetRPM,rightTargetRPM,leftSmoothedRPM,"
           "rightSmoothedRPM,stopped,leftActualRPM,rightActualRPM,leftBoostCurrent,rightBoostCurrent,mode\n");
    bench::Routine routine;
    bench::Inputs in;
    Motor m[2];
    m[1].drag = rightDrag;
    in.fresh[0] = in.fresh[1] = true;
    bool field = true;
    for (uint32_t now = 1000; now < 200000; now += 5)
    {
        in.nowMs = now;
        in.requested = field;
        in.button = abortAt >= 0 && now >= 1000 + static_cast<uint32_t>(abortAt);
        for (int i = 0; i < 2; ++i)
        {
            in.rpm[i] = m[i].rpm;
            in.current[i] = m[i].current;
        }
        const bench::Output out = routine.update(in);
        if (out.clearRequest) field = false;
        while (routine.hasLine())
        {
            host(now);
            fputs(routine.line(), stdout);
            routine.popLine();
        }
        if (now % 50 == 0)
        {
            host(now);
            printf("%u,0,0,0,0,0,0,0.0000,0.0000,nan,nan,nan,nan,1,%.4f,%.4f,0.0000,0.0000,DISARM\n", now, m[0].rpm, m[1].rpm);
            host(now);
            printf("DBG M %u %.1f %.1f 0.000 0.000 0.0 0.0 0.0 0.0 0 0 0 0 0.0 0.0 4.92\n", now, m[0].current, m[1].current);
        }
        for (auto &motor : m) motor.step(out, 0.005f);
        if (!routine.active() && !field && now > 2000)
            break;
    }
    return 0;
}
