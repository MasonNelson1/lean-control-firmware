// Bench test routine (src/bench_test.h): start gates, every abort, the field
// clearing, the full sequence against a simulated pair of motors, and that it
// can never run outside DISARM or with someone in the seat.
#include "bench_test.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bench;
static int passed = 0, failed = 0;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(std::string(#condition) + " line " + std::to_string(__LINE__)); } while (false)
static void test(const std::string &name, const std::function<void()> &body)
{
    try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
    catch (const std::exception &error) { ++failed; std::cout << "FAIL " << name << " -- " << error.what() << '\n'; }
}

static bool has(const std::vector<std::string> &lines, const std::string &text)
{
    for (const auto &l : lines)
        if (l.find(text) != std::string::npos) return true;
    return false;
}

// Wheels off: a first-order speed loop, brake current decelerates hard,
// released wheels coast down. Current = drag + a little per RPM, so a side's
// "binding" shows as extra current.
struct Motor
{
    float rpm = 0, current = 0;
    float drag = 1.0f;      // A at any speed
    float perRpm = 0.02f;   // A per RPM
    float tau = 0.12f;      // s
    float maxRpm = 1000;    // can't go faster (a stalled or weak side)
    bool stuck = false;     // won't slow down under braking (a runaway for timeout tests)
    void step(const Output &o, float dt)
    {
        switch (o.cmd)
        {
        case Cmd::Rpm:
        {
            const float target = std::max(-maxRpm, std::min(maxRpm, o.value));
            rpm += (target - rpm) * (dt / tau);
            current = (rpm >= 0 ? 1 : -1) * (drag + perRpm * fabsf(rpm)) + 0.5f * (target - rpm) / 10.0f;
            break;
        }
        case Cmd::Brake:
            if (!stuck)
            {
                const float dec = o.value * 40.0f * dt; // RPM per tick
                rpm = fabsf(rpm) <= dec ? 0 : rpm - (rpm > 0 ? dec : -dec);
            }
            current = -o.value * (rpm != 0);
            break;
        case Cmd::Release:
        case Cmd::None:
            if (!stuck)
            {
                const float dec = 30.0f * dt;
                rpm = fabsf(rpm) <= dec ? 0 : rpm - (rpm > 0 ? dec : -dec);
            }
            current = 0;
            break;
        }
    }
};

struct Rig
{
    Routine routine;
    Inputs in;
    Motor m[2];
    std::vector<std::string> lines;
    Output last;
    bool field = false; // the config field (RUN = true)
    uint32_t dtMs = 5;  // the control period

    Rig()
    {
        in.mode = common::DISARM;
        in.fresh[0] = in.fresh[1] = true;
    }
    Output tick()
    {
        in.requested = field;
        in.rpm[0] = m[0].rpm;
        in.rpm[1] = m[1].rpm;
        in.current[0] = m[0].current;
        in.current[1] = m[1].current;
        last = routine.update(in);
        if (last.clearRequest) field = false;
        while (routine.hasLine())
        {
            const std::string line = routine.line();
            CHECK(line.size() < Routine::MaxLine && line.back() == '\n' && line.rfind("DBG BENCH ", 0) == 0);
            lines.push_back(line);
            routine.popLine();
        }
        for (auto &motor : m) motor.step(last, dtMs / 1000.0f);
        in.nowMs += dtMs;
        return last;
    }
    // Runs until the routine goes idle or the time runs out; returns elapsed ms.
    uint32_t runUntilIdle(uint32_t limitMs = 200000)
    {
        const uint32_t start = in.nowMs;
        do tick(); while (routine.active() && in.nowMs - start < limitMs);
        return in.nowMs - start;
    }
    void start()
    {
        field = true;
        tick();
        CHECK(routine.running());
    }
    void runFor(uint32_t ms)
    {
        const uint32_t until = in.nowMs + ms;
        while (in.nowMs < until) tick();
    }
};

static void gates()
{
    test("all gates pass: starts with the first creep setpoint on both wheels", [] {
        Rig r;
        r.field = true;
        const Output o = r.tick();
        CHECK(r.routine.running());
        CHECK(o.cmd == Cmd::Rpm && o.value == 3.0f && !o.clearRequest);
        CHECK(r.field); // still RUN while running
        CHECK(has(r.lines, "begin steps=") && has(r.lines, "step=creep3 start kind=hold set=3.0"));
    });
    test("nothing happens while the field is OFF", [] {
        Rig r;
        for (int i = 0; i < 100; ++i) CHECK(r.tick().cmd == Cmd::None);
        CHECK(!r.routine.active() && r.lines.empty());
    });

    struct Gate { const char *why; std::function<void(Inputs &)> set; };
    const std::vector<Gate> refusals = {
        {"not-disarmed", [](Inputs &in) { in.mode = common::LEAN; }},
        {"not-disarmed", [](Inputs &in) { in.mode = common::REMOTE; }},
        {"not-disarmed", [](Inputs &in) { in.mode = common::ASSIST; }},
        {"button", [](Inputs &in) { in.button = true; }},
        {"seated", [](Inputs &in) { in.seated = true; }},
        {"safety-brake", [](Inputs &in) { in.safetyBrake = true; }},
        {"vesc-busy", [](Inputs &in) { in.vescBusy = true; }},
        {"vesc-silent-L", [](Inputs &in) { in.fresh[0] = false; }},
        {"vesc-silent-R", [](Inputs &in) { in.fresh[1] = false; }},
        {"fault-L", [](Inputs &in) { in.fault[0] = 4; }},
        {"fault-R", [](Inputs &in) { in.fault[1] = 1; }},
    };
    for (const auto &g : refusals)
        test(std::string("refuses to start: ") + g.why, [&g] {
            Rig r;
            g.set(r.in);
            r.field = true;
            const Output o = r.tick();
            CHECK(o.cmd == Cmd::None && o.clearRequest && !r.field && !r.routine.active());
            CHECK(has(r.lines, std::string("refused why=") + g.why));
            for (int i = 0; i < 50; ++i) CHECK(r.tick().cmd == Cmd::None); // and stays off
        });
    test("refuses to start: a wheel turning (|RPM| >= 3), either side, either way", [] {
        for (int side = 0; side < 2; ++side)
            for (float rpm : {3.0f, -3.0f, 40.0f, NAN})
            {
                Rig r;
                r.m[side].rpm = rpm;
                r.m[side].stuck = true;
                r.field = true;
                CHECK(r.tick().cmd == Cmd::None && !r.field);
                CHECK(has(r.lines, side ? "refused why=moving-R" : "refused why=moving-L"));
            }
        Rig r; // 2.9 RPM is "still"
        r.m[0].rpm = 2.9f;
        r.m[1].rpm = -2.9f;
        r.field = true;
        r.tick();
        CHECK(r.routine.running());
    });
    test("never runs in LEAN, REMOTE or ASSIST, or seated, however long RUN is held", [] {
        for (auto mode : {common::LEAN, common::REMOTE, common::ASSIST})
            for (bool seated : {false, true})
            {
                Rig r;
                r.in.mode = mode;
                r.in.seated = seated;
                for (int i = 0; i < 2000; ++i)
                {
                    r.field = true; // re-sent every tick
                    CHECK(r.tick().cmd == Cmd::None && !r.routine.active());
                }
            }
        Rig r;
        r.in.seated = true;
        for (int i = 0; i < 2000; ++i)
        {
            r.field = true;
            CHECK(r.tick().cmd == Cmd::None && !r.routine.active());
        }
    });
}

static void aborts()
{
    struct Abort { const char *why; std::function<void(Inputs &)> set; };
    const std::vector<Abort> reasons = {
        {"button", [](Inputs &in) { in.button = true; }},
        {"seated", [](Inputs &in) { in.seated = true; }},
        {"safety-brake", [](Inputs &in) { in.safetyBrake = true; }},
        {"vesc-silent-L", [](Inputs &in) { in.fresh[0] = false; }},
        {"vesc-silent-R", [](Inputs &in) { in.fresh[1] = false; }},
        {"fault-L", [](Inputs &in) { in.fault[0] = 2; }},
        {"fault-R", [](Inputs &in) { in.fault[1] = 7; }},
        // A fault that came and went between two polls still counts.
        {"fault-L", [](Inputs &in) { in.faultEpisodes[0] += 1; }},
        {"fault-R", [](Inputs &in) { in.faultEpisodes[1] += 1; }},
    };
    for (const auto &a : reasons)
        test(std::string("aborts on ") + a.why + ": brakes to a stop, releases, clears the field", [&a] {
            Rig r;
            r.start();
            r.runFor(12000); // into fwd30/fwd60: wheels spinning
            CHECK(r.routine.running() && fabsf(r.m[0].rpm) > 10);
            a.set(r.in);
            const Output o = r.tick();
            CHECK(o.cmd == Cmd::Brake && o.value == AbortBrakeA && o.clearRequest && !r.field);
            CHECK(has(r.lines, std::string("abort why=") + a.why));
            bool released = false;
            for (int i = 0; i < 2000 && r.routine.active(); ++i)
            {
                const Output b = r.tick();
                if (b.cmd == Cmd::Release) released = true;
                else CHECK(b.cmd == Cmd::Brake);
            }
            CHECK(!r.routine.active() && released && has(r.lines, std::string("released why=") + a.why));
            CHECK(r.tick().cmd == Cmd::None);
        });
    test("aborts when the field is set back to OFF", [] {
        Rig r;
        r.start();
        r.runFor(8000);
        r.field = false;
        const Output o = r.tick();
        CHECK(o.cmd == Cmd::Brake && has(r.lines, "abort why=cancelled"));
        r.runUntilIdle();
        CHECK(has(r.lines, "released why=cancelled") && fabsf(r.m[0].rpm) < StillRPM);
    });
    test("leaving DISARM mid-run stops at once and hands the motors to the new mode", [] {
        for (auto mode : {common::LEAN, common::REMOTE, common::ASSIST})
        {
            Rig r;
            r.start();
            r.runFor(10000);
            r.in.mode = mode;
            const Output o = r.tick();
            CHECK(o.cmd == Cmd::None && o.clearRequest && !r.routine.active() && !r.field);
            CHECK(has(r.lines, "abort why=mode") && has(r.lines, "released why=mode brakeMs=0"));
        }
    });
    test("a stop that never reaches still times out, aborts and releases after the abort brake limit", [] {
        Rig r;
        r.start();
        r.runFor(28000); // into fwd90 (25-30 s)
        r.m[0].stuck = true; // left keeps turning whatever we do
        r.runUntilIdle();
        CHECK(has(r.lines, "step=stopF sum") && has(r.lines, "abort why=timeout step=stopF"));
        CHECK(has(r.lines, "released why=timeout brakeMs=4000 still=0"));
        CHECK(!r.field && r.last.cmd == Cmd::Release);
    });
    test("spin-up that can't reach 90 RPM times out", [] {
        Rig r;
        r.m[1].maxRpm = 70; // a weak right side
        r.start();
        r.runUntilIdle();
        CHECK(has(r.lines, "abort why=timeout step=spin90"));
        CHECK(!has(r.lines, "step=brake start"));
    });
    test("RUN sent again while it brakes after an abort is ignored and cleared", [] {
        Rig r;
        r.start();
        r.runFor(12000);
        r.in.button = true;
        r.tick();
        r.in.button = false;
        r.field = true;
        r.tick();
        CHECK(r.last.cmd == Cmd::Brake || r.last.cmd == Cmd::Release);
        r.runUntilIdle();
        CHECK(!r.field && !r.routine.active());
        CHECK(r.tick().cmd == Cmd::None);
    });
}

static std::string valueOf(const std::string &line, const std::string &key)
{
    const auto at = line.find(" " + key + "=");
    if (at == std::string::npos) return "";
    const auto start = at + key.size() + 2;
    return line.substr(start, line.find_first_of(" \n", start) - start);
}

static void sequence()
{
    test("full run: every step logs start, summary and end; done; field cleared; 60-90 s", [] {
        Rig r;
        r.start();
        const uint32_t ms = r.runUntilIdle() + r.dtMs;
        CHECK(!r.routine.active() && !r.field && r.last.cmd == Cmd::Release && r.last.clearRequest);
        CHECK(has(r.lines, "done ms="));
        CHECK(ms >= 60000 && ms <= 90000);
        for (const auto &s : Steps)
        {
            const std::string name = std::string("step=") + s.name + " ";
            CHECK(has(r.lines, name + "start") && has(r.lines, name + "sum") && has(r.lines, name + "end"));
        }
        CHECK(!has(r.lines, "abort") && r.routine.droppedLines() == 0);
        CHECK(r.tick().cmd == Cmd::None);
    });
    test("holds report mean/sd per side; reached setpoints are ok=1", [] {
        Rig r;
        r.m[1].drag = 2.0f; // the right side binds: twice the drag current
        r.start();
        r.runUntilIdle();
        for (const auto &l : r.lines)
        {
            if (l.find("step=fwd60 sum") == std::string::npos) continue;
            CHECK(valueOf(l, "set") == "60.0" && valueOf(l, "okL") == "1" && valueOf(l, "okR") == "1");
            CHECK(std::stod(valueOf(l, "n")) >= 399); // 2 s at 5 ms
            CHECK(fabs(std::stod(valueOf(l, "rpmL")) - 60) < 0.5 && std::stod(valueOf(l, "rsdL")) < 0.5);
            const double iL = std::stod(valueOf(l, "iL")), iR = std::stod(valueOf(l, "iR"));
            CHECK(fabs(iL - 2.2) < 0.1 && fabs(iR - 3.2) < 0.1);
            return;
        }
        CHECK(false);
    });
    test("reverse holds command negative RPM and report it", [] {
        Rig r;
        r.start();
        bool sawReverse = false;
        while (r.routine.active())
        {
            const Output o = r.tick();
            if (o.cmd == Cmd::Rpm && o.value < 0) { sawReverse = true; CHECK(o.value >= -90); }
        }
        CHECK(sawReverse && has(r.lines, "step=rev90 sum set=-90.0"));
    });
    test("a side that can't reach the creep setpoint is ok=0", [] {
        Rig r;
        r.m[0].maxRpm = 0; // left doesn't move below min ERPM (or at all)
        r.start();
        r.runFor(3100);
        for (const auto &l : r.lines)
            if (l.find("step=creep3 sum") != std::string::npos)
            {
                CHECK(valueOf(l, "okL") == "0" && valueOf(l, "okR") == "1");
                return;
            }
        CHECK(false);
    });
    test("step response: time to 90%, overshoot and settling per side", [] {
        Rig r;
        r.m[1].tau = 0.30f; // right is slower
        r.start();
        r.runUntilIdle();
        for (const auto &l : r.lines)
        {
            if (l.find("step=up1 sum") == std::string::npos) continue;
            const long t90L = std::stol(valueOf(l, "t90L")), t90R = std::stol(valueOf(l, "t90R"));
            // First order: t90 = tau * ln(10) = 276 ms / 691 ms.
            CHECK(t90L > 230 && t90L < 320 && t90R > 620 && t90R < 760);
            CHECK(std::stod(valueOf(l, "ovsL")) == 0.0);
            CHECK(std::stol(valueOf(l, "setlL")) > 0 && std::stol(valueOf(l, "setlL")) < t90R);
            CHECK(valueOf(l, "from") == "0.0" && valueOf(l, "set") == "60.0");
            return;
        }
        CHECK(false);
    });
    test("step down 60 -> 0: t90 measured toward zero", [] {
        Rig r;
        r.start();
        r.runUntilIdle();
        for (const auto &l : r.lines)
            if (l.find("step=down1 sum") != std::string::npos)
            {
                CHECK(valueOf(l, "from") == "60.0" && valueOf(l, "set") == "0.0");
                const long t90 = std::stol(valueOf(l, "t90L"));
                CHECK(t90 > 230 && t90 < 320);
                return;
            }
        CHECK(false);
    });
    test("braking: brake current from 90 RPM, time to < 3 RPM per side", [] {
        Rig r;
        bool braked = false;
        r.start();
        while (r.routine.active())
        {
            const Output o = r.tick();
            if (std::string(r.routine.stepName()) == "brake") { braked = true; CHECK(o.cmd == Cmd::Brake && o.value == TestBrakeA); }
        }
        CHECK(braked);
        for (const auto &l : r.lines)
            if (l.find("step=brake sum") != std::string::npos)
            {
                CHECK(std::stod(valueOf(l, "rpm0L")) >= 81);
                const long stopL = std::stol(valueOf(l, "stopL"));
                // 90 RPM at 15 A * 40 RPM/s/A = 600 RPM/s: ~145 ms.
                CHECK(stopL > 100 && stopL < 200 && std::stol(valueOf(l, "stopR")) == stopL);
                return;
            }
        CHECK(false);
    });
    test("stops between groups brake, never command RPM against the spin", [] {
        Rig r;
        r.start();
        while (r.routine.active())
        {
            const Output o = r.tick();
            const std::string step = r.routine.stepName();
            if (step.rfind("stop", 0) == 0) CHECK(o.cmd == Cmd::Brake && o.value == StopBrakeA);
            if (o.cmd == Cmd::Rpm) // never a reverse command while still turning forward fast, or vice versa
                for (int i = 0; i < 2; ++i) CHECK(!(o.value < 0 && r.in.rpm[i] > 15) && !(o.value > 0 && r.in.rpm[i] < -15));
        }
    });
    test("runs again after finishing", [] {
        Rig r;
        r.start();
        r.runUntilIdle();
        r.lines.clear();
        r.start();
        r.runUntilIdle();
        CHECK(has(r.lines, "done ms=") && !r.field);
    });
}

int main()
{
    gates();
    aborts();
    sequence();
    std::cout << "TOTAL " << passed + failed << " / PASS " << passed << " / FAIL " << failed << '\n';
    return failed ? 1 : 0;
}
