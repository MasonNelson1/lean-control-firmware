#pragma once

// Wheels-off bench test (Charles, 2026-11-22: the quick-release pins had to be
// hammered out, so check the drivetrain for binding and collect per-side tuning
// data). Started from the laptop with
//     python.exe tools/chair_config.py set "bench" 1
// It runs only in DISARM with the seat empty, both VESCs reporting and both
// wheels still, drives LEFT AND RIGHT WITH THE SAME COMMAND so the sides can be
// compared, and logs `DBG BENCH` lines (tools/bench_report.py reads them).
//
// Pure logic, no Arduino: main.cpp feeds Inputs every control tick and sends
// the returned Output to both VESCs. Tests: tests/native/bench_test_test.cpp.
//
// Abort (any button, seat occupied, a VESC silent or faulted, the field set
// back to 0, a step timing out): brake both wheels to a stop, then release. If
// the mode somehow left DISARM the new mode owns the motors, so it just stops.
// Whenever it ends (done, aborted or refused) it asks main.cpp to set the
// field back to 0.

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Mode.h"

namespace bench
{
// The config field's values (DriveSettings.cpp, field 110, RAM only).
enum class Request : uint8_t
{
    Off = 0,
    Run = 1
};

constexpr float StillRPM = 3.0f;           // "not moving", the same threshold as the safety brake
constexpr uint32_t VescSilentMs = 200;     // STATUS_1 arrives at 50 Hz: 10 frames missed
constexpr uint32_t StatsWindowMs = 2000;   // statistics over the last 2 s of a hold
constexpr uint32_t SpinHoldMs = 1000;      // at 90 RPM this long before the brake test
constexpr float StopBrakeA = 8.0f;         // brake between steps (wheels off: little inertia)
constexpr float TestBrakeA = 15.0f;        // the braking test; the VESC braking limit is 20 A
constexpr float AbortBrakeA = 15.0f;       // same as main.cpp's SAFETY_BRAKE_MOVING_A
constexpr uint32_t AbortBrakeMaxMs = 4000; // then release even if a wheel still reads as moving
constexpr float ReachedFraction = 0.10f;   // a hold "reached" its setpoint within 10%...
constexpr float ReachedMinRPM = 1.5f;      // ...or 1.5 RPM, whichever is larger (creep steps)
constexpr float SettleBand = 0.05f;        // step settling band: 5% of the step size

enum class Kind : uint8_t
{
    Hold,       // RPM setpoint for `ms`; stats over the last StatsWindowMs
    StepChange, // RPM setpoint `from` -> `value` for `ms`; time to 90%, overshoot, settling
    SpinUp,     // RPM setpoint until both wheels reach 90% of it, then SpinHoldMs; timeout `ms`
    Stop,       // StopBrakeA until both wheels are still; timeout `ms`
    Brake       // `value` amps of brake current until both still; time per side; timeout `ms`
};

struct Step
{
    const char *name;
    Kind kind;
    float from;  // StepChange: where the step starts; Brake: nominal starting RPM
    float value; // RPM setpoint, or brake amps
    uint32_t ms; // hold time, or timeout
};

// About 65 s: 53 s of fixed holds plus the stops and the spin-up. Steady holds
// are 5 s so each speed has 3 s to settle before the 2 s stats window. 300 ERPM (the VESC's min
// ERPM) is 300 / 58.18 = 5.2 wheel RPM, so creep3 is below it, creep5 at it.
constexpr Step Steps[] = {
    {"creep3", Kind::Hold, 0, 3, 3000},
    {"creep5", Kind::Hold, 0, 5, 3000},
    {"creep8", Kind::Hold, 0, 8, 3000},
    {"fwd15", Kind::Hold, 0, 15, 5000},
    {"fwd30", Kind::Hold, 0, 30, 5000},
    {"fwd60", Kind::Hold, 0, 60, 5000},
    {"fwd90", Kind::Hold, 0, 90, 5000},
    {"stopF", Kind::Stop, 0, 0, 6000},
    {"rev15", Kind::Hold, 0, -15, 5000},
    {"rev30", Kind::Hold, 0, -30, 5000},
    {"rev60", Kind::Hold, 0, -60, 5000},
    {"rev90", Kind::Hold, 0, -90, 5000},
    {"stopR", Kind::Stop, 0, 0, 6000},
    {"up1", Kind::StepChange, 0, 60, 3000},
    {"down1", Kind::StepChange, 60, 0, 3000},
    {"stop1", Kind::Stop, 0, 0, 6000},
    {"up2", Kind::StepChange, 0, 60, 3000},
    {"down2", Kind::StepChange, 60, 0, 3000},
    {"stop2", Kind::Stop, 0, 0, 6000},
    {"spin90", Kind::SpinUp, 0, 90, 6000},
    {"brake", Kind::Brake, 90, TestBrakeA, 6000},
};
constexpr size_t StepCount = sizeof(Steps) / sizeof(Steps[0]);

struct Inputs
{
    uint32_t nowMs = 0;
    bool requested = false; // the config field is non-zero
    common::mode mode = common::DISARM;
    bool seated = false;
    bool button = false;      // any button held or pressed since the last tick
    bool safetyBrake = false; // main.cpp's automatic-disarm brake is latched
    bool vescBusy = false;    // a VESC settings write (vesc_apply.h) is running
    bool fresh[2] = {false, false};      // STATUS_1 within VescSilentMs, left/right
    uint8_t fault[2] = {0, 0};           // latest COMM_GET_VALUES fault code
    uint32_t faultEpisodes[2] = {0, 0};  // fault episodes since boot
    float rpm[2] = {0, 0};               // measured wheel RPM
    float current[2] = {0, 0};           // motor current, A
};

enum class Cmd : uint8_t
{
    None,  // not running: main.cpp does what DISARM always does
    Rpm,   // vescSetWheelRPM(value) on both
    Brake, // vescSetBrakeCurrent(value) on both
    Release // vescSetCurrent(0) on both
};

struct Output
{
    Cmd cmd = Cmd::None;
    float value = 0.0f;
    bool clearRequest = false; // set the config field back to 0
};

class Routine
{
public:
    static constexpr size_t MaxLine = 320;
    static constexpr size_t MaxLines = 12;

    Output update(const Inputs &in)
    {
        switch (phase_)
        {
        case Phase::Idle:
            return idle(in);
        case Phase::Running:
            if (const char *why = abortReason(in))
                return abort(why, in);
            return run(in);
        case Phase::Aborting:
        default:
            return braking(in);
        }
    }

    bool active() const { return phase_ != Phase::Idle; }
    bool running() const { return phase_ == Phase::Running; }
    const char *stepName() const { return phase_ == Phase::Running ? Steps[index_].name : "-"; }

    // DBG BENCH lines, oldest first. main.cpp sends them when Bluetooth has room,
    // so a summary is never dropped by a busy link (like vesc_monitor's faults).
    bool hasLine() const { return count_ != 0; }
    const char *line() const { return lines_[head_]; }
    void popLine()
    {
        if (!count_)
            return;
        head_ = (head_ + 1) % MaxLines;
        --count_;
    }
    uint32_t droppedLines() const { return dropped_; }

    // Why a start would be refused right now (nullptr = all gates pass).
    static const char *refusal(const Inputs &in)
    {
        if (in.mode != common::DISARM) return "not-disarmed";
        if (in.button) return "button";
        if (in.seated) return "seated";
        if (in.safetyBrake) return "safety-brake";
        if (in.vescBusy) return "vesc-busy";
        if (!in.fresh[0]) return "vesc-silent-L";
        if (!in.fresh[1]) return "vesc-silent-R";
        if (in.fault[0]) return "fault-L";
        if (in.fault[1]) return "fault-R";
        if (!(fabsf(in.rpm[0]) < StillRPM)) return "moving-L";
        if (!(fabsf(in.rpm[1]) < StillRPM)) return "moving-R";
        return nullptr;
    }

private:
    enum class Phase : uint8_t
    {
        Idle,
        Running,
        Aborting
    };

    struct Stats
    {
        uint32_t n = 0;
        double mean = 0, m2 = 0;
        void add(double x)
        {
            ++n;
            const double d = x - mean;
            mean += d / n;
            m2 += d * (x - mean);
        }
        double sd() const { return n > 1 ? sqrt(m2 / (n - 1)) : 0.0; }
    };

    struct Side
    {
        Stats current, rpm;
        long t90 = -1;          // ms from the step start to 90% of the change
        float peak = -1e9f;     // largest progress (1 = at the setpoint)
        long lastOutside = 0;   // ms: last sample outside the settling band
        bool endOutside = true; // the last sample was outside the band
        long stopMs = -1;       // Brake: ms to below StillRPM
        float startRpm = 0;
    };

    Output idle(const Inputs &in)
    {
        Output out;
        if (!in.requested)
            return out;
        out.clearRequest = true;
        if (const char *why = refusal(in))
        {
            log(in.nowMs, "refused why=%s mode=%s seated=%d button=%d brake=%d fresh=%d/%d fault=%u/%u rpmL=%.1f rpmR=%.1f",
                why, common::modeName(in.mode), in.seated, in.button, in.safetyBrake, in.fresh[0], in.fresh[1],
                in.fault[0], in.fault[1], in.rpm[0], in.rpm[1]);
            return out;
        }
        out.clearRequest = false;
        phase_ = Phase::Running;
        startMs_ = in.nowMs;
        episodes_[0] = in.faultEpisodes[0];
        episodes_[1] = in.faultEpisodes[1];
        log(in.nowMs, "begin steps=%u", static_cast<unsigned>(StepCount));
        begin(0, in);
        return command(Steps[0]);
    }

    const char *abortReason(const Inputs &in) const
    {
        if (in.mode != common::DISARM) return "mode";
        if (in.button) return "button";
        if (in.seated) return "seated";
        if (in.safetyBrake) return "safety-brake";
        if (in.vescBusy) return "vesc-busy";
        if (!in.fresh[0]) return "vesc-silent-L";
        if (!in.fresh[1]) return "vesc-silent-R";
        if (in.fault[0] || in.faultEpisodes[0] != episodes_[0]) return "fault-L";
        if (in.fault[1] || in.faultEpisodes[1] != episodes_[1]) return "fault-R";
        if (!in.requested) return "cancelled";
        return nullptr;
    }

    Output abort(const char *why, const Inputs &in)
    {
        log(in.nowMs, "abort why=%s step=%s t=%lu rpmL=%.1f rpmR=%.1f", why, Steps[index_].name,
            static_cast<unsigned long>(in.nowMs - startMs_), in.rpm[0], in.rpm[1]);
        Output out;
        out.clearRequest = true;
        if (in.mode != common::DISARM)
        {
            // Another mode owns the motors now; don't fight it.
            phase_ = Phase::Idle;
            log(in.nowMs, "released why=%s brakeMs=0 mode=%s", why, common::modeName(in.mode));
            return out;
        }
        phase_ = Phase::Aborting;
        abortMs_ = in.nowMs;
        snprintf(abortWhy_, sizeof(abortWhy_), "%s", why);
        out.cmd = Cmd::Brake;
        out.value = AbortBrakeA;
        return out;
    }

    Output braking(const Inputs &in)
    {
        Output out;
        const bool still = in.fresh[0] && in.fresh[1] && fabsf(in.rpm[0]) < StillRPM && fabsf(in.rpm[1]) < StillRPM;
        if (in.mode != common::DISARM || still || in.nowMs - abortMs_ >= AbortBrakeMaxMs)
        {
            phase_ = Phase::Idle;
            log(in.nowMs, "released why=%s brakeMs=%lu still=%d", abortWhy_,
                static_cast<unsigned long>(in.nowMs - abortMs_), still);
            out.cmd = in.mode == common::DISARM ? Cmd::Release : Cmd::None;
            out.clearRequest = true;
            return out;
        }
        out.cmd = Cmd::Brake;
        out.value = AbortBrakeA;
        return out;
    }

    static Output command(const Step &s)
    {
        Output out;
        switch (s.kind)
        {
        case Kind::Stop:
            out.cmd = Cmd::Brake;
            out.value = StopBrakeA;
            break;
        case Kind::Brake:
            out.cmd = Cmd::Brake;
            out.value = s.value;
            break;
        default:
            out.cmd = Cmd::Rpm;
            out.value = s.value;
            break;
        }
        return out;
    }

    static const char *kindName(Kind k)
    {
        switch (k)
        {
        case Kind::Hold: return "hold";
        case Kind::StepChange: return "step";
        case Kind::SpinUp: return "spinup";
        case Kind::Stop: return "stop";
        case Kind::Brake: return "brake";
        }
        return "?";
    }

    void begin(size_t index, const Inputs &in)
    {
        index_ = index;
        stepMs_ = in.nowMs;
        reachedMs_ = 0;
        reached_ = false;
        for (int i = 0; i < 2; ++i)
        {
            side_[i] = Side{};
            side_[i].startRpm = in.rpm[i];
        }
        const Step &s = Steps[index];
        log(in.nowMs, "step=%s start kind=%s set=%.1f from=%.1f rpmL=%.1f rpmR=%.1f", s.name, kindName(s.kind),
            s.value, s.from, in.rpm[0], in.rpm[1]);
    }

    void sample(const Step &s, const Inputs &in, long elapsed)
    {
        for (int i = 0; i < 2; ++i)
        {
            Side &d = side_[i];
            const float rpm = in.rpm[i];
            switch (s.kind)
            {
            case Kind::Hold:
            case Kind::StepChange:
                if (elapsed + static_cast<long>(StatsWindowMs) >= static_cast<long>(s.ms))
                {
                    d.current.add(in.current[i]);
                    d.rpm.add(rpm);
                }
                if (s.kind == Kind::StepChange)
                {
                    const float span = s.value - s.from;
                    const float progress = (rpm - s.from) / span;
                    if (d.t90 < 0 && progress >= 0.9f)
                        d.t90 = elapsed;
                    if (progress > d.peak)
                        d.peak = progress;
                    d.endOutside = fabsf(rpm - s.value) > SettleBand * fabsf(span);
                    if (d.endOutside)
                        d.lastOutside = elapsed;
                }
                break;
            case Kind::SpinUp:
                if (reached_)
                {
                    d.current.add(in.current[i]);
                    d.rpm.add(rpm);
                }
                break;
            case Kind::Brake:
                if (d.stopMs < 0 && fabsf(rpm) < StillRPM)
                    d.stopMs = elapsed;
                break;
            case Kind::Stop:
                break;
            }
        }
    }

    Output run(const Inputs &in)
    {
        const Step &s = Steps[index_];
        const long elapsed = static_cast<long>(in.nowMs - stepMs_);
        sample(s, in, elapsed);
        bool done = false, timedOut = false;
        switch (s.kind)
        {
        case Kind::Hold:
        case Kind::StepChange:
            done = elapsed >= static_cast<long>(s.ms);
            break;
        case Kind::SpinUp:
            if (!reached_ && fabsf(in.rpm[0]) >= 0.9f * fabsf(s.value) && fabsf(in.rpm[1]) >= 0.9f * fabsf(s.value) &&
                in.rpm[0] * s.value > 0 && in.rpm[1] * s.value > 0)
            {
                reached_ = true;
                reachedMs_ = elapsed;
            }
            done = reached_ && elapsed - reachedMs_ >= static_cast<long>(SpinHoldMs);
            timedOut = !reached_ && elapsed >= static_cast<long>(s.ms);
            break;
        case Kind::Stop:
            done = fabsf(in.rpm[0]) < StillRPM && fabsf(in.rpm[1]) < StillRPM;
            timedOut = !done && elapsed >= static_cast<long>(s.ms);
            break;
        case Kind::Brake:
            done = side_[0].stopMs >= 0 && side_[1].stopMs >= 0;
            timedOut = !done && elapsed >= static_cast<long>(s.ms);
            break;
        }
        if (timedOut)
        {
            summarize(s, in, elapsed); // what it got to, for the report
            return abort("timeout", in);
        }
        if (!done)
            return command(s);
        summarize(s, in, elapsed);
        log(in.nowMs, "step=%s end ms=%ld", s.name, elapsed);
        if (index_ + 1 >= StepCount)
        {
            phase_ = Phase::Idle;
            log(in.nowMs, "done ms=%lu", static_cast<unsigned long>(in.nowMs - startMs_));
            Output out;
            out.cmd = Cmd::Release;
            out.clearRequest = true;
            return out;
        }
        begin(index_ + 1, in);
        return command(Steps[index_]);
    }

    static bool reachedSetpoint(const Side &d, float set)
    {
        const float tolerance = fmaxf(ReachedFraction * fabsf(set), ReachedMinRPM);
        return d.rpm.n > 0 && fabsf(static_cast<float>(d.rpm.mean) - set) <= tolerance;
    }

    // One summary per step. Hold/step/spin-up: per side mean and sd of motor
    // current (iL/isdL, A) and RPM (rpmL/rsdL) over the stats window, ok = mean
    // RPM reached the setpoint. Step: t90 (ms to 90% of the change, -1 never),
    // ovs (overshoot past the setpoint, % of the step), setl (ms after the step
    // start of the last sample outside ±5%, -1 = still outside at the end).
    // Brake: stop (ms to below 3 RPM, -1 never) and the RPM it started from.
    void summarize(const Step &s, const Inputs &in, long elapsed)
    {
        const Side &l = side_[0], &r = side_[1];
        switch (s.kind)
        {
        case Kind::Hold:
        case Kind::SpinUp:
            log(in.nowMs,
                "step=%s sum set=%.1f n=%lu iL=%.2f isdL=%.2f rpmL=%.2f rsdL=%.2f okL=%d iR=%.2f isdR=%.2f rpmR=%.2f "
                "rsdR=%.2f okR=%d",
                s.name, s.value, static_cast<unsigned long>(l.rpm.n), l.current.mean, l.current.sd(), l.rpm.mean,
                l.rpm.sd(), reachedSetpoint(l, s.value), r.current.mean, r.current.sd(), r.rpm.mean, r.rpm.sd(),
                reachedSetpoint(r, s.value));
            break;
        case Kind::StepChange:
        {
            const auto ovs = [](const Side &d) { return d.peak > 1.0f ? (d.peak - 1.0f) * 100.0f : 0.0f; };
            const auto settle = [](const Side &d) { return d.endOutside ? -1L : d.lastOutside; };
            log(in.nowMs,
                "step=%s sum set=%.1f from=%.1f n=%lu iL=%.2f isdL=%.2f rpmL=%.2f rsdL=%.2f okL=%d t90L=%ld ovsL=%.1f "
                "setlL=%ld iR=%.2f isdR=%.2f rpmR=%.2f rsdR=%.2f okR=%d t90R=%ld ovsR=%.1f setlR=%ld",
                s.name, s.value, s.from, static_cast<unsigned long>(l.rpm.n), l.current.mean, l.current.sd(),
                l.rpm.mean, l.rpm.sd(), reachedSetpoint(l, s.value), l.t90, ovs(l), settle(l), r.current.mean,
                r.current.sd(), r.rpm.mean, r.rpm.sd(), reachedSetpoint(r, s.value), r.t90, ovs(r), settle(r));
            break;
        }
        case Kind::Brake:
            log(in.nowMs, "step=%s sum amps=%.1f rpm0L=%.1f rpm0R=%.1f stopL=%ld stopR=%ld", s.name, s.value,
                l.startRpm, r.startRpm, l.stopMs, r.stopMs);
            break;
        case Kind::Stop:
            log(in.nowMs, "step=%s sum amps=%.1f ms=%ld rpm0L=%.1f rpm0R=%.1f", s.name, StopBrakeA, elapsed,
                l.startRpm, r.startRpm);
            break;
        }
    }

    void log(uint32_t now, const char *format, ...) __attribute__((format(printf, 3, 4)))
    {
        if (count_ == MaxLines)
        {
            popLine(); // keep the newest
            ++dropped_;
        }
        char *line = lines_[(head_ + count_) % MaxLines];
        int n = snprintf(line, MaxLine, "DBG BENCH %lu ", static_cast<unsigned long>(now));
        va_list args;
        va_start(args, format);
        if (n > 0 && static_cast<size_t>(n) < MaxLine)
            n += vsnprintf(line + n, MaxLine - n, format, args);
        va_end(args);
        const size_t length = strnlen(line, MaxLine - 2);
        line[length] = '\n';
        line[length + 1] = 0;
        ++count_;
    }

    Phase phase_ = Phase::Idle;
    size_t index_ = 0;
    uint32_t startMs_ = 0, stepMs_ = 0, abortMs_ = 0;
    long reachedMs_ = 0;
    bool reached_ = false;
    uint32_t episodes_[2] = {0, 0};
    Side side_[2];
    char abortWhy_[24] = "";
    char lines_[MaxLines][MaxLine] = {};
    size_t head_ = 0, count_ = 0;
    uint32_t dropped_ = 0;
};
} // namespace bench
