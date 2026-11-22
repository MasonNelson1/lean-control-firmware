// Compiles the actual LeanDriveMode with simulated ADC readings; no motors.
#include "driveModes/LeanDriveMode.h"
#include "config/DriveSettings.h"
#include "telemetry/Telemetry.h"
#include <Arduino.h>
#include <cassert>
#include <cmath>
#include <cstdio>

void telemetry::Telemetry::logf(const char *, ...) {}

static void setGain(config::ConfigField<float> &field, float value) {
    assert(field.update(reinterpret_cast<const uint8_t *>(&value), sizeof(value)));
}
static void sensors(int f, int b, int l, int r) {
    sensorInputs[common::PIN_F] = f;
    sensorInputs[common::PIN_B] = b;
    sensorInputs[common::PIN_L] = l;
    sensorInputs[common::PIN_R] = r;
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.00001f; }

int main() {
    // These tests cover the plus layout's math (the corner layout is the default).
    const auto plus = static_cast<uint8_t>(lean::SensorLayout::Plus);
    assert(settings::sensorLayout.update(&plus, 1));
    setGain(settings::frontGain, 1);
    setGain(settings::rearGain, 1);
    setGain(settings::leftGain, 1);
    setGain(settings::rightGain, 1);
    LeanDriveMode mode;
    mode.updateResponseCurve();
    mode.resetDynamicGains();
    sensors(1000, 1000, 1000, 1000);
    auto targets = mode.getTargets();
    assert(targets.left == 0 && targets.right == 0); // calibration gate
    mode.calibrate();
    assert(mode.getUserWeightEstimate() == 1000);
    targets = mode.getTargets();
    assert(targets.left == 0 && targets.right == 0);

    const auto &speedKnee = settings::leanForewardBackResponse.get();
    const auto &turnKnee = settings::leanSideToSideResponse.get();
    const float halfSpeed = std::pow(0.5f, std::log(speedKnee[1]) / std::log(speedKnee[0]));
    const float halfTurn = std::pow(0.5f, std::log(turnKnee[1]) / std::log(turnKnee[0]));
    sensors(1500, 1000, 1000, 1000);
    mode.getTargets();
    assert(near(mode.getTelemetry().speed, halfSpeed));
    sensors(1000, 1500, 1000, 1000);
    mode.getTargets();
    assert(near(mode.getTelemetry().speed, -halfSpeed));
    sensors(1000, 1000, 1000, 1500);
    mode.getTargets();
    assert(near(mode.getTelemetry().turn, halfTurn));
    sensors(1000, 1000, 1500, 1000);
    mode.getTargets();
    assert(near(mode.getTelemetry().turn, -halfTurn));

    // Both readings exceed the former per-sensor cap: preserve their difference.
    sensors(3000, 2000, 2000, 3000);
    targets = mode.getTargets();
    const auto sample = mode.getTelemetry();
    assert(sample.front == 2000 && sample.back == 1000);
    assert(sample.left == 1000 && sample.right == 2000);
    assert(near(sample.speed, 1) && near(sample.turn, 1));
    assert(std::fabs(targets.left) <= 1 && std::fabs(targets.right) <= 1);
    sensors(4095, 1000, 1000, 1000);
    mode.getTargets();
    assert(near(mode.getTelemetry().speed, 1)); // normalized output still clamped
    sensors(1000, 4095, 1000, 1000);
    mode.getTargets();
    assert(near(mode.getTelemetry().speed, -1));

    // A raw reading at either ADC rail on any one sensor is flagged; resting readings are not.
    sensors(372, 437, 372, 372);
    mode.sampleSensors();
    assert(!mode.sensorAtRail());
    for (uint8_t pin : {common::PIN_F, common::PIN_B, common::PIN_L, common::PIN_R})
        for (int stuck : {4095, 0})
        {
            sensors(372, 437, 372, 372);
            sensorInputs[pin] = stuck;
            mode.sampleSensors();
            assert(mode.sensorAtRail());
        }
    sensors(372, 437, 372, 372);
    mode.sampleSensors();
    assert(!mode.sensorAtRail()); // not latched: the disarm in main.cpp is what sticks

    // Zero weight cannot cause division by zero or nonfinite commands.
    sensors(0, 0, 0, 0);
    mode.calibrate();
    sensors(2000, 0, 0, 2000);
    targets = mode.getTargets();
    assert(targets.left == 0 && targets.right == 0);
    assert(mode.getTelemetry().speed == 0 && mode.getTelemetry().turn == 0);
    for (uint16_t id = 20; id <= 23; ++id) assert(!settings::registry().find(id));
    assert(!settings::registry().findGroup(4));
    puts("Lean weight normalization and retired saturation settings tests passed");
}
