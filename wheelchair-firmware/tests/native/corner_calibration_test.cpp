// Corner-layout calibration and sensor safety: the relaxed-posture press, reach
// limits and lifetime, dead-channel detection (at calibration and mid-ride),
// rail thresholds, calibration averaging and the automatic empty-seat zero.
// Compiles the actual LeanDriveMode, lean_layout and config sources; no motors.
#include "driveModes/LeanDriveMode.h"
#include "driveModes/lean_layout.h"
#include "config/DriveSettings.h"
#include "telemetry/Telemetry.h"
#include <Arduino.h>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>

static std::string messages;
void telemetry::Telemetry::logf(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    assert(length > 0 && length < 256);
    messages += line;
}

namespace
{
    using lean::SensorLayout;

    bool near(float a, float b, float tolerance = 0.0001f) { return std::fabs(a - b) <= tolerance; }
    bool logged(const char *text) { return messages.find(text) != std::string::npos; }

    void sensors(const lean::Counts &raw)
    {
        sensorInputs[common::PIN_F] = raw[lean::F];
        sensorInputs[common::PIN_B] = raw[lean::B];
        sensorInputs[common::PIN_L] = raw[lean::L];
        sensorInputs[common::PIN_R] = raw[lean::R];
    }

    void setLayout(SensorLayout layout)
    {
        const auto value = static_cast<uint8_t>(layout);
        assert(settings::sensorLayout.update(&value, 1));
    }

    template <class T> void set(config::ConfigField<T> &field, T value)
    {
        assert(field.update(reinterpret_cast<const uint8_t *>(&value), sizeof(value)));
    }

    // E9 (2026-11-22) hold averages, raw F/B/L/R. F = back-left, B = back-right,
    // L = front-left, R = front-right.
    constexpr lean::Counts PassiveNeutral{{849, 716, 677, 810}};
    constexpr lean::Counts ActiveNeutral{{1038, 864, 657, 790}};
    constexpr lean::Counts Forward{{927, 756, 1058, 1390}};
    constexpr lean::Counts Right{{1002, 1180, 524, 865}};
    constexpr lean::Counts Left{{1348, 773, 787, 698}};
    constexpr lean::Counts BackTouch{{1093, 891, 469, 484}};
    constexpr lean::Counts EmptyPlate{{391, 406, 427, 400}};
    constexpr lean::Counts Reseated{{383, 396, 420, 404}}; // empty plate after reseating it
    constexpr int AmpOffset = 372;                         // what a channel with no force reads

    LeanDriveMode freshMode()
    {
        setLayout(SensorLayout::Corner);
        settings::registry().defaults();
        LeanDriveMode mode;
        mode.updateResponseCurve();
        mode.resetDynamicGains();
        return mode;
    }

    // Ramp to a posture over 40 ticks, as a rider does (see corner_layout_test).
    lean::Counts lastRaw{};
    lean::Command drive(LeanDriveMode &mode, const lean::Counts &raw, MotorTargets *targets = nullptr)
    {
        const lean::Counts from = lastRaw[0] ? lastRaw : raw;
        MotorTargets out{};
        for (int step = 1; step <= 40; ++step)
        {
            lean::Counts now;
            for (size_t i = 0; i < lean::Channels; ++i)
                now[i] = from[i] + (raw[i] - from[i]) * step / 40;
            sensors(now);
            out = mode.getTargets();
            assert(!mode.centreOfPressureJumped());
        }
        lastRaw = raw;
        if (targets)
            *targets = out;
        return {mode.getTelemetry().speed, mode.getTelemetry().turn};
    }

    // A short press of the calibrate button: press, then release before 300 ms.
    void shortPress(LeanDriveMode &mode, const lean::Counts &raw, uint32_t at)
    {
        sensors(raw);
        mode.beginCalibration(at);
        mode.sampleSensors();
        assert(!mode.updateCalibration(at + 100, false));
        lastRaw = raw;
    }

    // Press, hold while leaning through `leans`, release. Returns updateCalibration's result.
    bool holdPress(LeanDriveMode &mode, const lean::Counts &start, std::initializer_list<lean::Counts> leans, uint32_t at)
    {
        sensors(start);
        mode.beginCalibration(at);
        uint32_t now = at + 300;
        for (const auto &raw : leans)
        {
            sensors(raw);
            mode.sampleSensors();
            mode.updateCalibration(now += 10, true);
        }
        sensors(start);
        mode.sampleSensors();
        lastRaw = start;
        return mode.updateCalibration(now + 10, false);
    }

    void relaxedPosture()
    {
        auto mode = freshMode();
        shortPress(mode, ActiveNeutral, 1000);
        const int weight = mode.getUserWeightEstimate();
        assert(!mode.getPassive().valid);
        // Before a relaxed press, slumping creeps forward (+0.139 speed).
        auto cmd = drive(mode, PassiveNeutral);
        assert(near(cmd.speed, 0.13933f));
        // The LED blinks slowly while a second press would record the relaxed posture.
        assert(!mode.calibrationLEDOn(1200) && mode.calibrationLEDOn(1600) && !mode.calibrationLEDOn(2100));

        // Second press, relaxed, within 10 s: records the posture, keeps neutral and W.
        messages.clear();
        shortPress(mode, PassiveNeutral, 6000);
        assert(mode.getPassive().valid);
        assert(near(mode.getPassive().at.x, -0.1543f) && near(mode.getPassive().at.y, -0.8971f));
        assert(near(mode.getNeutral().at.y, -1.6349f) && mode.getUserWeightEstimate() == weight);
        assert(logged("** Relaxed posture x=-0.15 y=-0.90 in (+0.74 in from neutral); no speed from y=-1.78 to -0.75 **"));
        assert(!mode.calibrationLEDOn(6600)); // window closed

        // replay_cop.py --passive passive-neutral gives the same numbers.
        MotorTargets t;
        cmd = drive(mode, PassiveNeutral, &t);
        assert(cmd.speed == 0 && cmd.turn == 0 && t.left == 0 && t.right == 0); // no creep
        cmd = drive(mode, ActiveNeutral, &t);
        assert(cmd.speed == 0 && cmd.turn == 0);
        cmd = drive(mode, Forward, &t);
        assert(near(cmd.speed, 0.49544f) && near(cmd.turn, 0.06865f)); // forward reach now from relaxed
        cmd = drive(mode, BackTouch, &t);
        assert(near(cmd.speed, -0.99116f) && cmd.turn == 0);
        drive(mode, ActiveNeutral); // BackTouch latched the reverse-turn flip; neutral releases it
        cmd = drive(mode, Left, &t);
        assert(near(cmd.speed, -0.00265f) && near(cmd.turn, -1));

        // A third press is a fresh neutral, and drops the relaxed posture.
        shortPress(mode, ActiveNeutral, 8000);
        assert(!mode.getPassive().valid);

        // After the window, a second press is a new neutral, not a relaxed posture.
        shortPress(mode, ActiveNeutral, 20000);
        shortPress(mode, PassiveNeutral, 20000 + lean::PassiveWindowMs + 1);
        assert(!mode.getPassive().valid && near(mode.getNeutral().at.y, -0.8971f));
        // Exactly at the window's end still counts.
        shortPress(mode, ActiveNeutral, 50000);
        shortPress(mode, PassiveNeutral, 50000 + lean::PassiveWindowMs);
        assert(mode.getPassive().valid);

        // A "relaxed" press made mid-lean is rejected; neutral stays as it was.
        shortPress(mode, ActiveNeutral, 80000);
        messages.clear();
        shortPress(mode, Forward, 81000);
        assert(!mode.getPassive().valid && near(mode.getNeutral().at.y, -1.6349f));
        assert(logged("Relaxed posture rejected: +0.49/+2.06 in from neutral is a lean (limit 1.50); keeping neutral only."));

        // Re-entering LEAN (reset) closes the window.
        shortPress(mode, ActiveNeutral, 100000);
        mode.reset();
        shortPress(mode, PassiveNeutral, 101000);
        assert(!mode.getPassive().valid && near(mode.getNeutral().at.y, -0.8971f));

        // The plus layout has no relaxed posture: every press is neutral.
        setLayout(SensorLayout::Plus);
        LeanDriveMode plus;
        plus.updateResponseCurve();
        plus.resetDynamicGains();
        shortPress(plus, ActiveNeutral, 1000);
        assert(!plus.calibrationLEDOn(1600)); // no blink
        shortPress(plus, PassiveNeutral, 2000);
        assert(!plus.getPassive().valid && plus.getUserWeightEstimate() == (849 + 716 + 677 + 810) / 4);
        setLayout(SensorLayout::Corner);
    }

    void reachWithRelaxedPosture()
    {
        // Upright press, then relaxed press held through all four leans: the
        // forward reach is measured from the relaxed posture.
        auto mode = freshMode();
        shortPress(mode, ActiveNeutral, 1000);
        assert(holdPress(mode, PassiveNeutral, {Forward, BackTouch, Left, Right}, 3000));
        assert(mode.getPassive().valid);
        const auto reach = mode.activeReach();
        assert(near(reach.forward, (0.4265f + 0.8971f) * lean::ReachUseFraction, 0.001f)); // from relaxed, not upright
        assert(near(reach.back, (3.5248f - 1.6349f) * lean::ReachUseFraction, 0.001f));    // from upright
        assert(near(reach.left, (1.3047f + 0.2976f) * lean::ReachUseFraction, 0.001f)); // both sides use the smaller one
        assert(near(reach.right, (1.3047f + 0.2976f) * lean::ReachUseFraction, 0.001f));
    }

    void reachLimitsAndLifetime()
    {
        auto mode = freshMode();
        // A lazy hold that only slumps (0.74") can't become full forward.
        messages.clear();
        assert(holdPress(mode, ActiveNeutral, {PassiveNeutral, BackTouch, Left, Right}, 1000));
        assert(logged("Reach calibration rejected: forward reached 0.74 in, required 1.20."));
        assert(mode.activeReach().forward == lean::DefaultReachForward);
        // Configured reach can't be set below 1.2" either.
        const float short_ = 1.19f;
        assert(!settings::reachForward.update(reinterpret_cast<const uint8_t *>(&short_), 4));

        assert(!mode.takeReachSave()); // a rejected hold saves nothing
        assert(settings::reachForward.get() == lean::DefaultReachForward);

        // A good hold sets this rider's reach, and writes it into the reach settings
        // so it survives power cycles (main.cpp saves them into the active profile).
        const float forward = 2.0614f * lean::ReachUseFraction;
        const float back = (3.5248f - 1.6349f) * lean::ReachUseFraction;
        const float side = (1.3047f + 0.2976f) * lean::ReachUseFraction; // both sides use the smaller one
        auto settingsHoldReach = [&] {
            return near(settings::reachForward.get(), forward, 0.001f) && near(settings::reachBack.get(), back, 0.001f) &&
                   near(settings::reachLeft.get(), side, 0.001f) && near(settings::reachRight.get(), side, 0.001f);
        };
        assert(holdPress(mode, ActiveNeutral, {Forward, BackTouch, Left, Right}, 2000));
        assert(near(mode.activeReach().forward, forward, 0.001f));
        assert(settingsHoldReach());
        assert(mode.takeReachSave()); // one save per calibration...
        assert(!mode.takeReachSave());
        // ...which a new neutral press (same rider, re-entering LEAN) keeps.
        mode.reset();
        shortPress(mode, ActiveNeutral, 5000);
        assert(near(mode.activeReach().forward, forward, 0.001f));
        assert(!mode.takeReachSave());
        mode.settingsChanged(); // an unrelated change keeps it
        assert(near(mode.activeReach().forward, forward, 0.001f));
        // Editing a reach setting changes just that direction; the rest stay calibrated.
        set(settings::reachLeft, 1.5f);
        assert(near(mode.activeReach().forward, forward, 0.001f) && mode.activeReach().left == 1.5f);
        mode.settingsChanged();
        assert(logged("Reach setting changed: hold-calibrated reach cleared."));
        assert(near(mode.activeReach().forward, forward, 0.001f) && mode.activeReach().left == 1.5f);

        // The calibrated reach lives in the settings, so an empty seat for 3 s (a new
        // rider) and a profile change leave it alone; only loading defaults drops it.
        settings::registry().defaults();
        assert(holdPress(mode, ActiveNeutral, {Forward, BackTouch, Left, Right}, 16000));
        assert(settingsHoldReach());
        assert(mode.takeReachSave());
        auto tick = [&](const lean::Counts &raw, uint32_t from, uint32_t to) {
            sensors(raw);
            for (uint32_t now = from; now <= to; now += 50)
            {
                mode.sampleSensors();
                mode.trackEmptyPlate(now, true);
            }
        };
        tick(ActiveNeutral, 20000, 20000);
        tick(EmptyPlate, 20050, 22000);   // out of the seat for 2 s...
        tick(ActiveNeutral, 22050, 22050); // ...and back
        assert(near(mode.activeReach().forward, forward, 0.001f));
        messages.clear();
        tick(EmptyPlate, 22100, 25000);    // 2,950 ms after the last seated sample
        assert(near(mode.activeReach().forward, forward, 0.001f));
        tick(EmptyPlate, 25050, 25050);    // 3,000 ms
        assert(logged("Seat empty: hold-calibrated reach cleared; using the configured reach."));
        assert(near(mode.activeReach().forward, forward, 0.001f) && settingsHoldReach());
        assert(!mode.takeReachSave());

        // A profile change keeps the calibrated settings until the new profile loads its own.
        assert(holdPress(mode, ActiveNeutral, {Forward, BackTouch, Left, Right}, 40000));
        assert(mode.takeReachSave());
        mode.resetDynamicGains();
        assert(near(mode.activeReach().forward, forward, 0.001f));
        settings::registry().defaults();
        assert(mode.activeReach().forward == lean::DefaultReachForward);
    }

    void deadChannelAtCalibration()
    {
        // A channel already dead (reading the amp offset) when the rider calibrates.
        for (size_t dead = 0; dead < lean::Channels; ++dead)
        {
            auto mode = freshMode();
            lean::Counts raw = ActiveNeutral;
            raw[dead] = AmpOffset;
            messages.clear();
            shortPress(mode, raw, 1000);
            assert(!mode.getNeutral().valid);
            assert(logged("Calibration rejected: channel "));
            assert(logged(lean::CornerNames[dead]));
            assert(mode.calibrationLEDOn(5000)); // LED says "not calibrated"
            sensors(Forward);
            const auto t = mode.getTargets();
            assert(t.left == 0 && t.right == 0 && mode.getTelemetry().stopped);
        }
        // A merely light corner is fine: E9 neutral's lightest carries 230 of 1,725.
        auto mode = freshMode();
        shortPress(mode, ActiveNeutral, 1000);
        assert(mode.getNeutral().valid && !mode.calibrationLEDOn(20000));
        // Court 2026-11-22: sitting back, front-left carried ~75 of ~2,050. Accepted now
        // (the old 5% share rule rejected it). 18:04: even 11-28 counts on front-left
        // must pass; only a raw reading at the amp zero (< DeadChannelRawMax) is dead.
        shortPress(mode, {{1344, 1076, 427 + 75, 755}}, 30000);
        assert(mode.getNeutral().valid);
        shortPress(mode, {{1500, 1300, 427 + 20, 800}}, 60000);
        assert(mode.getNeutral().valid);
        shortPress(mode, {{1500, 1300, lean::DeadChannelRawMax - 6, 800}}, 90000);
        assert(!mode.getNeutral().valid);
    }

    void deadChannelMidRide()
    {
        // MaxCopStep was raised to 2.5" (2026-11-22) after the 0.75" check misfired
        // during normal riding. It now catches gross jumps, such as both back corners
        // dropping out at once (the CoP leaps to the front pair), not one dead channel.
        for (const auto &posture : {ActiveNeutral, Forward})
        {
            auto mode = freshMode();
            shortPress(mode, ActiveNeutral, 1000);
            drive(mode, posture);
            lean::Counts raw = posture;
            raw[lean::F] = AmpOffset; // back-left
            raw[lean::B] = AmpOffset; // back-right
            sensors(raw);
            const auto t = mode.getTargets();
            assert(mode.centreOfPressureJumped());
            assert(t.left == 0 && t.right == 0 && mode.getTelemetry().stopped);
            // The flag is per tick; the disarm in main.cpp is what sticks.
            mode.getTargets();
            assert(!mode.centreOfPressureJumped());
        }
        // Known limit now: a single dead channel steps the CoP 0.22-1.96", under the
        // threshold, so it isn't caught by this check (rail check: raw < 340 still is).
        auto mode = freshMode();
        shortPress(mode, ActiveNeutral, 1000);
        drive(mode, ActiveNeutral);
        lean::Counts raw = ActiveNeutral;
        raw[lean::L] = AmpOffset;
        sensors(raw);
        mode.getTargets();
        assert(!mode.centreOfPressureJumped());

        // A brisk healthy lean (neutral to full forward in 100 ms, ≈20 in/s, twice
        // E9's fastest) never trips it.
        mode = freshMode();
        shortPress(mode, ActiveNeutral, 1000);
        for (int step = 1; step <= 20; ++step)
        {
            lean::Counts now;
            for (size_t i = 0; i < lean::Channels; ++i)
                now[i] = ActiveNeutral[i] + (Forward[i] - ActiveNeutral[i]) * step / 20;
            sensors(now);
            mode.getTargets();
            assert(!mode.centreOfPressureJumped());
        }
        // A calibration press restarts the comparison: the rider may have moved
        // while it was down (here, a new neutral taken leaning forward).
        mode = freshMode();
        shortPress(mode, ActiveNeutral, 1000);
        drive(mode, ActiveNeutral);
        shortPress(mode, Forward, 30000);
        sensors(Forward);
        mode.getTargets();
        assert(!mode.centreOfPressureJumped());
    }

    void railThresholds()
    {
        // Both layouts: below 341 or above 3999 counts on any channel is a fault.
        static_assert(LeanDriveMode::SensorRailLow == 340 && LeanDriveMode::SensorRailHigh == 4000, "");
        for (auto layout : {SensorLayout::Plus, SensorLayout::Corner})
        {
            setLayout(layout);
            LeanDriveMode mode;
            for (size_t channel = 0; channel < lean::Channels; ++channel)
                for (auto [raw, railed] : std::initializer_list<std::pair<int, bool>>{
                         {340, true}, {341, false}, {369, false}, {3999, false}, {4000, true}})
                {
                    lean::Counts r = ActiveNeutral;
                    r[channel] = raw;
                    sensors(r);
                    mode.sampleSensors(/*seatOnly=*/true);
                    assert(mode.sensorAtRail() == railed);
                }
        }
        setLayout(SensorLayout::Corner);
    }

    // calibrate() averages calibrationSamples reads per channel, rounding half up.
    int readsPerPin[64]{};
    int alternating(uint8_t pin)
    {
        const int n = readsPerPin[pin]++;
        if (pin == common::PIN_F)
            return n % 2 ? 1001 : 1000; // mean 1000.5 → 1001
        if (pin == common::PIN_B)
            return 800 + n;             // 800..819 → mean 809.5 → 810
        if (pin == common::PIN_L)
            return n < 5 ? 700 : 600;   // 5×700 + 15×600 → 625
        return 790;
    }

    void calibrationAveraging()
    {
        auto mode = freshMode();
        assert(settings::calibrationSamples.get() == 20);
        analogReadHook = alternating;
        mode.calibrate();
        analogReadHook = nullptr;
        for (uint8_t pin : {common::PIN_F, common::PIN_B, common::PIN_L, common::PIN_R})
            assert(readsPerPin[pin] == 20);
        assert(logged("floor F=1001 B=810 L=625 R=790"));
        assert(mode.getUserWeightEstimate() == (1001 + 810 + 625 + 790) / 4);
    }

    void emptyPlateTracker()
    {
        const lean::Counts configured = EmptyPlate;
        lean::EmptyPlateTracker tracker;
        assert(tracker.baseline(configured) == configured && !tracker.adopted(configured));

        // Steady for the whole window: adopt the mean.
        uint32_t now = 1000;
        for (; now < 1000 + lean::EmptyZeroWindowMs; now += 50)
            assert(!tracker.update(Reseated, now, true, configured));
        assert(tracker.update(Reseated, now, true, configured));
        assert(tracker.baseline(configured) == Reseated && tracker.adopted(configured));

        // Noise within EmptySteadyCounts averages; a step beyond it restarts the window.
        lean::EmptyPlateTracker noisy;
        now = 0;
        for (int i = 0; now <= lean::EmptyZeroWindowMs; ++i, now += 50)
        {
            lean::Counts r = Reseated;
            r[lean::B] += i % 2 ? 6 : -6; // spans 12 counts: still steady
            if (noisy.update(r, now, true, configured))
                assert(noisy.baseline(configured) == Reseated);
        }
        assert(noisy.adopted(configured));
        lean::EmptyPlateTracker bumped;
        now = 0;
        bool adopted = false;
        for (int i = 0; i <= 60; ++i, now += 50)
        {
            lean::Counts r = Reseated;
            if (i == 30)
                r[lean::L] += 13; // someone touched the seat
            adopted = bumped.update(r, now, true, configured) || adopted;
        }
        assert(!adopted); // 30 + 30 samples: neither window lasted 3 s

        // Too far from the configured baseline: something is on the seat.
        for (auto [shift, accepted] : std::initializer_list<std::pair<int, bool>>{{30, true}, {31, false}, {-30, true}, {-31, false}})
        {
            lean::EmptyPlateTracker guard;
            lean::Counts r = configured;
            r[lean::R] += shift;
            bool got = false;
            for (now = 0; now <= lean::EmptyZeroWindowMs; now += 50)
                got = guard.update(r, now, true, configured) || got;
            assert(got == accepted && guard.adopted(configured) == accepted);
        }

        // Not eligible (armed, or someone seated) stops the window.
        lean::EmptyPlateTracker interrupted;
        for (now = 0; now <= lean::EmptyZeroWindowMs; now += 50)
            assert(!interrupted.update(Reseated, now, now != 1500, configured));

        // millis() rollover mid-window.
        lean::EmptyPlateTracker rolling;
        bool rolled = false;
        for (uint32_t t = UINT32_MAX - 1000, i = 0; i <= 61; ++i, t += 50)
            rolled = rolling.update(Reseated, t, true, configured) || rolled;
        assert(rolled);

        // A different configured baseline (a dashboard edit) wins over the zero.
        lean::Counts edited = configured;
        edited[lean::F] = 395;
        assert(tracker.baseline(edited) == edited && !tracker.adopted(edited));
    }

    void autoZeroInLeanMode()
    {
        auto mode = freshMode();
        auto settle = [&](const lean::Counts &raw, bool disarmed, uint32_t from) {
            sensors(raw);
            for (uint32_t now = from; now <= from + lean::EmptyZeroWindowMs; now += 50)
            {
                mode.sampleSensors();
                mode.trackEmptyPlate(now, disarmed);
            }
        };
        // Armed (not DISARM): no zero.
        settle(Reseated, false, 1000);
        assert(mode.emptyPlateInUse() == EmptyPlate);
        // Disarmed, empty and steady: zeroed in RAM, settings untouched.
        messages.clear();
        settle(Reseated, true, 10000);
        assert(mode.emptyPlateInUse() == Reseated);
        assert(settings::emptyPlateF.get() == 391);
        assert(logged("Empty plate re-zeroed: F=383 B=396 L=420 R=404 (configured 391/406/427/400)."));
        // The CoP now reads the reseated plate as zero load.
        sensors(Reseated);
        mode.sampleSensors();
        assert(mode.getTelemetry().front == 0 && mode.getTelemetry().right == 0);
        messages.clear();
        mode.publishCornerStatus();
        assert(logged(" empty=383/396/420/404 auto=1\n"));
        // Re-zeroing to the same values doesn't repeat the message.
        messages.clear();
        settle(Reseated, true, 20000);
        assert(!logged("re-zeroed"));
        // Someone seated: no zero, even disarmed.
        settle(ActiveNeutral, true, 30000);
        assert(mode.emptyPlateInUse() == Reseated);
        // Plus layout: never.
        auto plus = freshMode();
        setLayout(SensorLayout::Plus);
        sensors(Reseated);
        for (uint32_t now = 0; now <= 4000; now += 50)
        {
            plus.sampleSensors();
            plus.trackEmptyPlate(now, true);
        }
        assert(plus.emptyPlateInUse() == EmptyPlate);
        setLayout(SensorLayout::Corner);
        // A dashboard edit of a baseline replaces the zero.
        set(settings::emptyPlateF, uint16_t{390});
        assert(mode.emptyPlateInUse()[lean::F] == 390 && mode.emptyPlateInUse()[lean::B] == 406);
        settings::emptyPlateF.reset();
    }
} // namespace

int main()
{
    relaxedPosture();
    reachWithRelaxedPosture();
    reachLimitsAndLifetime();
    deadChannelAtCalibration();
    deadChannelMidRide();
    railThresholds();
    calibrationAveraging();
    emptyPlateTracker();
    autoZeroInLeanMode();
    puts("Corner calibration, relaxed posture, dead-channel, rail and empty-seat zero tests passed");
}
