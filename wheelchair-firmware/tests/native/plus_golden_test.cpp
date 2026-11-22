// Proves the plus-layout lean math is bit-for-bit what it was before the sensor
// layout abstraction (lean_layout.h) was added.
//
// The same file compiles against the old code (no lean_layout.h) and the new
// code (selects SensorLayout::Plus). It drives LeanDriveMode through calibration,
// dynamic calibration, setting changes and thousands of pseudo-random ADC
// samples, and hashes every output's exact bits (targets, telemetry, gains,
// seat and rail flags).
//
//   ./plus_golden_test            compare against plus_golden_expected.h
//   ./plus_golden_test --generate print a new plus_golden_expected.h
//
// plus_golden_expected.h was generated from commit 8712aef, the last commit
// before the layout abstraction, so a pass means the plus layout still behaves
// exactly as it did there. Never regenerate it from code you are trying to verify.
#include "driveModes/LeanDriveMode.h"
#include "config/DriveSettings.h"
#include "telemetry/Telemetry.h"
#if __has_include("driveModes/lean_layout.h")
#include "driveModes/lean_layout.h"
#define HAS_SENSOR_LAYOUT 1
#endif
#include <Arduino.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

void telemetry::Telemetry::logf(const char *, ...) {}

namespace
{
    struct Run
    {
        uint64_t hash = 1469598103934665603ull; // FNV-1a over every output byte
        int samples = 0;
        std::vector<std::array<uint32_t, 4>> head; // first samples, kept readable for debugging

        void bytes(const void *data, size_t size)
        {
            const auto *p = static_cast<const uint8_t *>(data);
            for (size_t i = 0; i < size; ++i)
                hash = (hash ^ p[i]) * 1099511628211ull;
        }
        template <class T> void add(T value) { bytes(&value, sizeof(value)); }
    };

    uint32_t rng = 0x2545F491u;
    uint32_t next()
    {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    }
    int rawSample()
    {
        // Mostly the chair's real range, sometimes anywhere on the 12-bit scale.
        return next() % 8 == 0 ? static_cast<int>(next() % 4096) : 340 + static_cast<int>(next() % 1700);
    }

    void sensors(int f, int b, int l, int r)
    {
        sensorInputs[common::PIN_F] = f;
        sensorInputs[common::PIN_B] = b;
        sensorInputs[common::PIN_L] = l;
        sensorInputs[common::PIN_R] = r;
    }

    uint32_t bits(float value)
    {
        uint32_t out;
        std::memcpy(&out, &value, sizeof(out));
        return out;
    }

    void record(Run &run, LeanDriveMode &mode, const MotorTargets &targets)
    {
        const auto &t = mode.getTelemetry();
        run.add(bits(targets.left));
        run.add(bits(targets.right));
        run.add(t.front);
        run.add(t.back);
        run.add(t.left);
        run.add(t.right);
        run.add(bits(t.speed));
        run.add(bits(t.turn));
        run.add(t.stopped);
        run.add(mode.isSeated());
        run.add(mode.sensorAtRail());
        run.add(mode.getUserWeightEstimate());
        for (float g : mode.getDynamicGains())
            run.add(bits(g));
        ++run.samples;
        if (run.head.size() < 12)
            run.head.push_back({bits(targets.left), bits(targets.right), bits(t.speed), bits(t.turn)});
    }

    // Half the samples land within ±120 counts of the calibration point, where
    // the output is unsaturated and the deadband, curves and turn mix all matter.
    int near(int centre) { return centre - 120 + static_cast<int>(next() % 241); }

    void randomDrive(Run &run, LeanDriveMode &mode, int count, std::array<int, 4> centre)
    {
        for (int i = 0; i < count; ++i)
        {
            if (i % 2)
                sensors(near(centre[0]), near(centre[1]), near(centre[2]), near(centre[3]));
            else
                sensors(rawSample(), rawSample(), rawSample(), rawSample());
            if (i % 7 == 0)
                mode.sampleSensors(/*seatOnly=*/true); // seat-only refresh must not touch telemetry
            record(run, mode, mode.getTargets());
        }
    }

    template <size_t N> void setProfile(config::ProfileField<N> &field, std::array<float, N> values)
    {
        // Reuse the field's own encoding so the editor tag byte is right.
        uint8_t data[config::MaxValueSize];
        config::Writer out(data, sizeof(data));
        field.value(out);
        std::memcpy(data + 1, values.data(), sizeof(float) * N);
        if (!field.update(data, out.size()))
        {
            puts("profile update rejected");
            std::exit(2);
        }
    }

    void setFloat(config::ConfigField<float> &field, float value)
    {
        if (!field.update(reinterpret_cast<const uint8_t *>(&value), sizeof(value)))
        {
            puts("setting update rejected");
            std::exit(2);
        }
    }

    // Scenario list. Each returns its own hash so a mismatch points at one area.
    Run uncalibrated()
    {
        Run run;
        LeanDriveMode mode;
        mode.updateResponseCurve();
        mode.resetDynamicGains();
        randomDrive(run, mode, 300, {{800, 800, 800, 800}});
        return run;
    }

    Run calibratedDefaults()
    {
        Run run;
        LeanDriveMode mode;
        mode.updateResponseCurve();
        mode.resetDynamicGains();
        sensors(1038, 864, 657, 790); // E9 active-neutral raw averages
        mode.calibrate();
        randomDrive(run, mode, 3000, {{1038, 864, 657, 790}});
        // Hand-picked edges: neutral, rails, reverse-flip band, hard reverse.
        const int edges[][4] = {{1038, 864, 657, 790}, {4095, 864, 657, 790}, {0, 0, 0, 0},
                                {900, 1300, 700, 760}, {800, 1500, 600, 900}, {700, 2000, 900, 600},
                                {2000, 400, 400, 2000}, {372, 372, 372, 372}, {1038, 864, 1200, 790}};
        for (const auto &e : edges)
        {
            sensors(e[0], e[1], e[2], e[3]);
            record(run, mode, mode.getTargets());
        }
        return run;
    }

    Run dynamicGains()
    {
        Run run;
        LeanDriveMode mode;
        mode.updateResponseCurve();
        mode.resetDynamicGains();
        sensors(900, 800, 700, 760);
        mode.beginCalibration(1000);
        const int peaks[][4] = {{1500, 650, 700, 760}, {820, 1400, 700, 760}, {900, 800, 1300, 600}, {900, 800, 600, 1250}};
        uint32_t now = 1300;
        for (const auto &p : peaks)
        {
            sensors(p[0], p[1], p[2], p[3]);
            mode.sampleSensors();
            mode.updateCalibration(now += 10, true);
        }
        mode.sampleSensors();
        mode.updateCalibration(now += 10, false);
        record(run, mode, mode.getTargetsFromSample());
        randomDrive(run, mode, 3000, {{900, 800, 700, 760}});
        return run;
    }

    Run changedSettings()
    {
        Run run;
        setFloat(settings::deadband, 0.12f);
        setProfile(settings::leanForewardBackResponse, {{0.3f, 0.2f}});
        setProfile(settings::leanSideToSideResponse, {{0.5f, 0.25f}});
        setProfile(settings::leanTurnMix, {{0.0f, 0.2f, 0.5f, 0.5f, 1.0f, 1.0458f}});
        setFloat(settings::frontGain, 1.5f);
        setFloat(settings::rightGain, 0.75f);
        LeanDriveMode mode;
        mode.updateResponseCurve();
        mode.resetDynamicGains();
        sensors(700, 650, 600, 640);
        mode.calibrate();
        randomDrive(run, mode, 3000, {{700, 650, 600, 640}});
        settings::registry().defaults();
        return run;
    }
} // namespace

#include "plus_golden_expected.h"

int main(int argc, char **argv)
{
#ifdef HAS_SENSOR_LAYOUT
    lean::selectLayoutForTest(lean::SensorLayout::Plus);
#endif
    const Run runs[] = {uncalibrated(), calibratedDefaults(), dynamicGains(), changedSettings()};
    const char *names[] = {"uncalibrated", "calibratedDefaults", "dynamicGains", "changedSettings"};
    if (argc > 1 && std::strcmp(argv[1], "--generate") == 0)
    {
        puts("// Generated by plus_golden_test --generate. See plus_golden_test.cpp before editing.");
        puts("#pragma once");
        puts("#include <cstdint>");
        puts("namespace golden {");
        printf("constexpr uint64_t Hashes[] = {");
        for (const auto &run : runs)
            printf("0x%016llxull, ", static_cast<unsigned long long>(run.hash));
        puts("};");
        puts("// First samples of calibratedDefaults: left, right, speed, turn as float bits.");
        printf("constexpr uint32_t CalibratedHead[][4] = {\n");
        for (const auto &h : runs[1].head)
            printf("    {0x%08x, 0x%08x, 0x%08x, 0x%08x},\n", h[0], h[1], h[2], h[3]);
        puts("};");
        puts("} // namespace golden");
        return 0;
    }
    int failures = 0;
    for (size_t i = 0; i < 4; ++i)
        if (runs[i].hash != golden::Hashes[i])
        {
            printf("plus layout changed: %s hash %016llx, expected %016llx\n", names[i],
                   static_cast<unsigned long long>(runs[i].hash), static_cast<unsigned long long>(golden::Hashes[i]));
            ++failures;
        }
    for (size_t i = 0; i < runs[1].head.size(); ++i)
        for (size_t j = 0; j < 4; ++j)
            if (runs[1].head[i][j] != golden::CalibratedHead[i][j])
            {
                printf("calibratedDefaults sample %zu value %zu: %08x, expected %08x\n", i, j,
                       runs[1].head[i][j], golden::CalibratedHead[i][j]);
                ++failures;
            }
    if (failures)
        return 1;
    int samples = 0;
    for (const auto &run : runs)
        samples += run.samples;
    printf("Plus layout matches commit 8712aef bit for bit (%d samples)\n", samples);
}
