// Compile with the lean/config host stubs and the actual LeanDriveMode/config sources.
#include "driveModes/LeanDriveMode.h"
#include "config/DriveSettings.h"
#include "telemetry/Telemetry.h"
#include <Arduino.h>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

static std::string messages;
void telemetry::Telemetry::logf(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    messages += line;
}

static void sensors(int f, int b, int l, int r)
{
    sensorInputs[common::PIN_F] = f;
    sensorInputs[common::PIN_B] = b;
    sensorInputs[common::PIN_L] = l;
    sensorInputs[common::PIN_R] = r;
}

static bool sample(LeanDriveMode &mode, uint32_t now, bool held = true)
{
    mode.sampleSensors();
    return mode.updateCalibration(now, held);
}

static void checkGains(const LeanDriveMode &mode, float f, float b, float l, float r)
{
    const std::array<float, 4> expected{{f, b, l, r}};
    for (size_t i = 0; i < expected.size(); ++i)
        assert(std::fabs(mode.getDynamicGains()[i] - expected[i]) < 0.00001f);
}

int main()
{
    // These tests cover the plus layout's dynamic gains (the corner layout is the default).
    const auto plus = static_cast<uint8_t>(lean::SensorLayout::Plus);
    assert(settings::sensorLayout.update(&plus, 1));
    LeanDriveMode mode;
    mode.updateResponseCurve();
    mode.resetDynamicGains();
    const auto configured = mode.getDynamicGains();
    assert(mode.calibrationLEDOn(0));

    // A short press calibrates neutral and keeps the configured gains.
    sensors(1000, 1000, 1000, 1000);
    mode.beginCalibration(100);
    assert(!sample(mode, 399, false));
    assert(!mode.calibrationActive());
    assert(!mode.calibrationLEDOn(399));
    assert(mode.getUserWeightEstimate() == 1000);
    assert(mode.getDynamicGains() == configured);

    // Peaks use floor-subtracted ADC readings, independently of the active gains.
    mode.beginCalibration(1000);
    sensors(4095, 4095, 4095, 4095);
    sample(mode, 1299);
    assert(!mode.calibrationLEDOn(1299));
    sensors(1100, 1000, 1000, 1000);
    sample(mode, 1300);
    assert(mode.calibrationLEDOn(1300));
    assert(!mode.calibrationLEDOn(1400));
    assert(mode.calibrationLEDOn(1500));
    sensors(1000, 1200, 1000, 1000);
    sample(mode, 1400);
    sensors(1000, 1000, 1400, 1000);
    sample(mode, 1500);
    sensors(1000, 1000, 1000, 1800);
    sample(mode, 1600);
    assert(mode.getTelemetry().stopped);
    const auto stopped = mode.getTargetsFromSample();
    assert(stopped.left == 0 && stopped.right == 0);
    // A sample taken after release must not contribute to the peaks.
    sensors(4095, 4095, 4095, 4095);
    assert(sample(mode, 1700, false));
    assert(!sample(mode, 1705, false)); // completion is reported once
    checkGains(mode, 10, 5, 2.5f, 1.25f);
    assert(!mode.calibrationActive());
    assert(!mode.calibrationLEDOn(1700));
    assert(messages.find("DYNAMIC_GAINS F=10.0000 B=5.0000 L=2.5000 R=1.2500") != std::string::npos);
    sensors(1100, 1200, 1400, 1800);
    mode.sampleSensors();
    const auto &telemetry = mode.getTelemetry();
    assert(telemetry.front == 1000 && telemetry.back == 1000);
    assert(telemetry.left == 1000 && telemetry.right == 1000);

    // Recalibration changes neutral and weight, but keeps the dynamic gains.
    sensors(1200, 1200, 1200, 1200);
    mode.calibrate();
    assert(mode.getUserWeightEstimate() == 1200);
    checkGains(mode, 10, 5, 2.5f, 1.25f);
    mode.reset();
    checkGains(mode, 10, 5, 2.5f, 1.25f);

    // A single sensor below 10% rejects all four candidates.
    mode.beginCalibration(2000);
    sensors(1320, 1500, 1600, 1319);
    sample(mode, 2300);
    assert(sample(mode, 2400, false)); // rejected attempts also require Disarm
    checkGains(mode, 10, 5, 2.5f, 1.25f);
    assert(messages.find("right maximum 119, required 120.0") != std::string::npos);

    // Cancellation retains both the new neutral calibration and the prior gains.
    sensors(1200, 1200, 1200, 1200);
    mode.beginCalibration(3000);
    sensors(1800, 1800, 1800, 1800);
    sample(mode, 3300);
    mode.cancelCalibration();
    assert(!mode.calibrationActive());
    assert(!sample(mode, 3400, false));
    assert(!mode.calibrationLEDOn(3400));
    checkGains(mode, 10, 5, 2.5f, 1.25f);

    // A delayed first update (e.g. long neutral averaging) enters collection.
    sensors(1200, 1200, 1200, 1200);
    mode.beginCalibration(4000);
    sensors(1320, 1200, 1200, 1200);
    sample(mode, 5000);
    sensors(1200, 1320, 1200, 1200);
    sample(mode, 5005);
    sensors(1200, 1200, 1320, 1200);
    sample(mode, 5010);
    sensors(1200, 1200, 1200, 1320);
    sample(mode, 5015);
    sample(mode, 5100, false);
    checkGains(mode, 10, 10, 10, 10); // exact threshold is accepted

    // Hold timing and LED timing survive uint32_t millis() rollover.
    sensors(1000, 1000, 1000, 1000);
    const uint32_t start = UINT32_MAX - 100;
    mode.beginCalibration(start);
    sample(mode, start + 299);
    assert(!mode.calibrationLEDOn(start + 299));
    sensors(1500, 1000, 1000, 1000);
    sample(mode, start + 300);
    sensors(1000, 1500, 1000, 1000);
    sample(mode, start + 305);
    sensors(1000, 1000, 1500, 1000);
    sample(mode, start + 310);
    sensors(1000, 1000, 1000, 1500);
    sample(mode, start + 315);
    assert(mode.calibrationLEDOn(start + 300));
    assert(!mode.calibrationLEDOn(start + 400));
    sample(mode, start + 500, false);
    checkGains(mode, 2, 2, 2, 2);

    // Pair each raw peak with its simultaneous opposite reading, before gains.
    sensors(1000, 1000, 1000, 1000);
    mode.beginCalibration(7000);
    sensors(1400, 900, 1000, 1000); // larger difference, but not the front maximum
    sample(mode, 7300);
    sensors(1500, 1200, 1000, 1000); // front peak 500, opposite 200
    sample(mode, 7305);
    sensors(1100, 1600, 1000, 1000); // back peak 600, opposite 100
    sample(mode, 7310);
    sensors(1000, 1000, 1400, 1100); // left peak 400, opposite 100
    sample(mode, 7315);
    sensors(1000, 1000, 1150, 1800); // right peak 800, opposite 150
    sample(mode, 7320);
    sensors(1500, 900, 1000, 1000); // tied front maximum retains the first peak sample
    sample(mode, 7325);
    sample(mode, 7330, false);
    checkGains(mode, 1000.0f / 300, 2, 1000.0f / 300, 1000.0f / 650);

    // Signed opposite readings below neutral contribute to the difference.
    sensors(1000, 1000, 1000, 1000);
    mode.beginCalibration(8000);
    sensors(1400, 900, 1000, 1000);
    sample(mode, 8300);
    sensors(900, 1400, 1000, 1000);
    sample(mode, 8305);
    sensors(1000, 1000, 1400, 900);
    sample(mode, 8310);
    sensors(1000, 1000, 900, 1400);
    sample(mode, 8315);
    sample(mode, 8320, false);
    checkGains(mode, 2, 2, 2, 2);

    // Peaks can meet the existing 10% threshold but have invalid pair differences.
    sensors(1000, 1000, 1000, 1000);
    mode.beginCalibration(9000);
    sensors(1500, 1600, 1400, 1400);
    sample(mode, 9300);
    sample(mode, 9310, false);
    checkGains(mode, 2, 2, 2, 2);
    assert(messages.find("front difference at peak -100 must be positive") != std::string::npos);
    assert(messages.find("left difference at peak 0 must be positive") != std::string::npos);

    // Zero weight never produces infinite or zero gains.
    sensors(0, 0, 0, 0);
    mode.beginCalibration(10000);
    sensors(100, 100, 100, 100);
    sample(mode, 10300);
    sample(mode, 10400, false);
    checkGains(mode, 2, 2, 2, 2);

    // The startup/profile hook restores the configured defaults explicitly.
    mode.resetDynamicGains();
    assert(mode.getDynamicGains() == configured);
    puts("Dynamic calibration regression tests passed");
}
