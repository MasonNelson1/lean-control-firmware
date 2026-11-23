// Corner-layout lean math: centre of pressure, the neutral/reach mapping,
// LeanDriveMode on the corner layout, and switching between layouts.
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
#include <string>

static std::string messages;
void telemetry::Telemetry::logf(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    assert(length > 0 && length < 256); // the firmware drops longer lines entirely
    messages += line;
}

namespace
{
    using lean::SensorLayout;

    bool near(float a, float b, float tolerance = 0.0001f) { return std::fabs(a - b) <= tolerance; }

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

    // E9 (2026-11-22) hold averages, raw F/B/L/R, rounded like calibrate().
    // F = back-left, B = back-right, L = front-left, R = front-right.
    constexpr lean::Counts PassiveNeutral{{849, 716, 677, 810}};
    constexpr lean::Counts ActiveNeutral{{1038, 864, 657, 790}};
    constexpr lean::Counts Forward{{927, 756, 1058, 1390}};
    constexpr lean::Counts Right{{1002, 1180, 524, 865}};
    constexpr lean::Counts Left{{1348, 773, 787, 698}};
    constexpr lean::Counts BackTouch{{1093, 891, 469, 484}};
    constexpr lean::Counts EmptyPlate{{391, 406, 427, 400}};

    void centreOfPressureMath()
    {
        const lean::Gains unity{{1, 1, 1, 1}};
        // Force is counts above the empty plate times gain, floored at zero.
        const auto forces = lean::cornerForces({{500, 400, 427, 1000}}, EmptyPlate, {{1, 2, 1, 0.5f}});
        assert(forces[lean::F] == 109 && forces[lean::B] == 0 && forces[lean::L] == 0 && forces[lean::R] == 300);

        // One loaded corner puts the centre of pressure exactly on it.
        for (size_t i = 0; i < lean::Channels; ++i)
        {
            std::array<float, 4> one{};
            one[i] = 500;
            const auto cop = lean::centreOfPressure(one);
            assert(cop.valid && cop.at.x == lean::CornerPositions[i].x && cop.at.y == lean::CornerPositions[i].y);
        }
        // Equal load sits at the average of the four corners.
        const auto even = lean::centreOfPressure({{100, 100, 100, 100}});
        assert(near(even.at.x, (-5.75f + 5.3125f - 5.75f + 5.375f) / 4));
        assert(near(even.at.y, (-4.1875f - 4.25f + 3.0f + 2.9375f) / 4));
        assert(near(even.total, 400));
        // Load split 3:1 between front-right and back-left lands a quarter of the way.
        const auto split = lean::centreOfPressure({{100, 0, 0, 300}});
        assert(near(split.at.x, (100 * -5.75f + 300 * 5.375f) / 400));

        // Too little load can't be located; never divide by (nearly) zero.
        assert(!lean::centreOfPressure({{0, 0, 0, 0}}).valid);
        assert(!lean::centreOfPressure({{10, 10, 10, lean::MinCopTotal - 30.5f}}).valid);
        assert(lean::centreOfPressure({{10, 10, 10, lean::MinCopTotal - 30}}).valid);

        // The E9 active-neutral hold matches experiments/E9-E10-rectangle.html.
        const auto neutral = lean::centreOfPressure(lean::cornerForces(ActiveNeutral, EmptyPlate, unity));
        assert(near(neutral.at.x, -0.2976f) && near(neutral.at.y, -1.6349f) && neutral.total == 1725);
        // Gains act as a correction: halving back-right moves the CoP left and forward.
        const auto halfBR = lean::centreOfPressure(lean::cornerForces(ActiveNeutral, EmptyPlate, {{1, 2, 1, 1}}));
        assert(halfBR.at.x > neutral.at.x + 0.5f && halfBR.at.y < neutral.at.y);
    }

    void mappingMath()
    {
        // Inside the deadzone: nothing, including exactly at its edge.
        assert(lean::axisLean(0.0f, 2, 1.9f, 0.25f) == 0);
        assert(lean::axisLean(0.25f, 2, 1.9f, 0.25f) == 0);
        assert(lean::axisLean(-0.25f, 2, 1.9f, 0.25f) == 0);
        // A straight ramp from the edge to full reach, so there's no step.
        assert(near(lean::axisLean(0.26f, 2, 1.9f, 0.25f), 0.01f / 1.75f));
        assert(near(lean::axisLean(1.125f, 2, 1.9f, 0.25f), 0.5f));
        assert(near(lean::axisLean(2.0f, 2, 1.9f, 0.25f), 1));
        // Each side uses its own reach, and output clamps beyond it.
        assert(near(lean::axisLean(-1.9f, 2, 1.9f, 0.25f), -1));
        assert(near(lean::axisLean(-1.075f, 2, 1.9f, 0.25f), -0.5f));
        assert(lean::axisLean(5.0f, 2, 1.9f, 0.25f) == 1 && lean::axisLean(-5.0f, 2, 1.9f, 0.25f) == -1);
        // A reach inside the deadzone can't divide by zero or flip sign.
        assert(lean::axisLean(1.0f, 0.2f, 1.9f, 0.25f) == 0);
        assert(lean::axisLean(1.0f, 0.25f, 1.9f, 0.25f) == 0);
        // Zero deadzone is plain proportional.
        assert(near(lean::axisLean(0.4f, 1.6f, 1.7f, 0), 0.25f));

        // Directions: forward and right are positive, back and left negative.
        const lean::Reach reach{2.0f, 1.9f, 1.7f, 1.6f};
        const auto zone = lean::neutralZone({-0.3f, -1.64f}, nullptr, 0.25f, 0.15f);
        assert(zone.x0 == -0.3f && zone.yBack == -1.64f && zone.yForward == -1.64f);
        assert(zone.xDeadzone == 0.25f && zone.yDeadzone == 0.25f); // no relaxed posture: one deadzone
        auto cmd = lean::cornerCommand({-0.3f, 0.36f}, zone, reach);
        assert(near(cmd.speed, 1) && cmd.turn == 0);
        cmd = lean::cornerCommand({-0.3f, -3.54f}, zone, reach);
        assert(near(cmd.speed, -1) && cmd.turn == 0);
        cmd = lean::cornerCommand({1.3f, -1.64f}, zone, reach);
        assert(cmd.speed == 0 && near(cmd.turn, 1));
        cmd = lean::cornerCommand({-2.0f, -1.64f}, zone, reach);
        assert(cmd.speed == 0 && near(cmd.turn, -1));
        // Without a relaxed posture this is exactly axisLean on the offset from neutral.
        for (float y = -4.0f; y <= 1.0f; y += 0.0625f)
            assert(lean::cornerCommand({-0.3f, y}, zone, reach).speed == lean::axisLean(y - -1.64f, 2.0f, 1.9f, 0.25f));

        // With a relaxed posture 0.74" forward: no speed anywhere from upright to
        // relaxed ± the margin, forward reach from the relaxed point, back reach
        // still from upright, turning unchanged.
        const lean::Point relaxed{-0.15f, -0.90f};
        const auto both = lean::neutralZone({-0.3f, -1.64f}, &relaxed, 0.25f, 0.15f);
        assert(both.x0 == -0.3f && both.yBack == -1.64f && both.yForward == -0.90f);
        assert(both.xDeadzone == 0.25f && both.yDeadzone == 0.15f);
        for (float y : {-1.79f, -1.64f, -1.2f, -0.90f, -0.751f})
            assert(lean::cornerCommand({-0.3f, y}, both, reach).speed == 0);
        assert(lean::cornerCommand({-0.3f, -0.74f}, both, reach).speed > 0);
        assert(near(lean::cornerCommand({-0.3f, -0.90f + 1.075f}, both, reach).speed, 0.5f));
        assert(near(lean::cornerCommand({-0.3f, -0.90f + 2.0f}, both, reach).speed, 1));
        assert(near(lean::cornerCommand({-0.3f, -1.64f - 1.9f}, both, reach).speed, -1));
        assert(lean::cornerCommand({-0.3f, -1.81f}, both, reach).speed < 0);
        assert(near(lean::cornerCommand({1.3f, -1.2f}, both, reach).turn, 1));
        // A relaxed posture behind upright works the same way round.
        const lean::Point behind{-0.3f, -2.0f};
        const auto reversed = lean::neutralZone({-0.3f, -1.64f}, &behind, 0.25f, 0.15f);
        assert(reversed.yBack == -2.0f && reversed.yForward == -1.64f);

        // Boxes for telemetry: {left, right, back, front}.
        const auto dead = lean::deadBox(both);
        assert(near(dead.left, -0.55f) && near(dead.right, -0.05f) && near(dead.back, -1.79f) && near(dead.front, -0.75f));
        const auto full = lean::fullLeanBox(both, reach);
        assert(near(full.left, -2.0f) && near(full.right, 1.3f) && near(full.back, -3.54f) && near(full.front, 1.10f));

        // Shared shaping: with no deadband a tiny input still passes through the
        // curve, and the reverse-turn flip and hard-reverse cut are intact.
        auto shaped = lean::shapeCommand({0.5f, 0.5f}, 0, 2, 2);
        assert(near(shaped.speed, 0.25f) && near(shaped.turn, 0.25f));
        shaped = lean::shapeCommand({-0.6f, 0.5f}, 0, 1, 1);
        assert(near(shaped.speed, -0.6f) && near(shaped.turn, -0.5f)); // flipped below -0.3
        shaped = lean::shapeCommand({-0.3f, 0.5f}, 0, 1, 1);
        assert(near(shaped.turn, 0.5f)); // not flipped at exactly -0.3
        shaped = lean::shapeCommand({-0.95f, 0.5f}, 0, 1, 1);
        assert(shaped.turn == 0); // turning off in hard reverse
        // Corner layout flips reverse turning from -0.1 (court test 2026-11-22).
        assert(lean::shapeCommand({-0.2f, 0.5f}, 0, 1, 1, lean::CornerReverseFlipAt).turn < 0);
        assert(lean::shapeCommand({-0.05f, 0.5f}, 0, 1, 1, lean::CornerReverseFlipAt).turn > 0);
        assert(lean::shapeCommand({-0.2f, 0.5f}, 0, 1, 1).turn > 0); // plus default still -0.3
        shaped = lean::shapeCommand({0.04f, -0.04f}, 0.05f, 1, 1);
        assert(shaped.speed == 0 && shaped.turn == 0); // plus deadband still works

        assert(std::string(lean::layoutName(SensorLayout::Plus)) == "PLUS");
        assert(std::string(lean::layoutName(SensorLayout::Corner)) == "CORNER");
    }

    LeanDriveMode freshMode()
    {
        LeanDriveMode mode;
        mode.updateResponseCurve();
        mode.resetDynamicGains();
        return mode;
    }

    // Moves the readings to `raw` the way a rider does: over 40 control ticks
    // (200 ms at 5 ms), so the CoP never steps further than lean::MaxCopStep in
    // one tick. Returns the speed/turn and targets at the final posture.
    lean::Counts lastRaw{};
    lean::Command drive(LeanDriveMode &mode, const lean::Counts &raw, MotorTargets *targets = nullptr)
    {
        constexpr int Steps = 40;
        const lean::Counts from = lastRaw[0] ? lastRaw : raw;
        MotorTargets out{};
        for (int step = 1; step <= Steps; ++step)
        {
            lean::Counts now;
            for (size_t i = 0; i < lean::Channels; ++i)
                now[i] = from[i] + (raw[i] - from[i]) * step / Steps;
            sensors(now);
            out = mode.getTargets();
            assert(!mode.centreOfPressureJumped());
        }
        lastRaw = raw;
        if (targets)
            *targets = out;
        return {mode.getTelemetry().speed, mode.getTelemetry().turn};
    }

    // The E9 holds through LeanDriveMode, calibrated in active neutral. The
    // expected numbers are what experiments/tools/replay_cop.py computes for the
    // same hold-average counts, so the script and the firmware agree.
    void e9Holds()
    {
        setLayout(SensorLayout::Corner);
        auto mode = freshMode();
        MotorTargets t;

        sensors(ActiveNeutral);
        assert(mode.getTargets().left == 0 && mode.getTelemetry().stopped); // calibration gate
        mode.calibrate();
        assert(mode.getNeutral().valid);
        assert(near(mode.getNeutral().at.x, -0.2976f) && near(mode.getNeutral().at.y, -1.6349f));
        assert(messages.find("Neutral centre of pressure x=-0.30 y=-1.63") != std::string::npos);

        auto cmd = drive(mode, ActiveNeutral, &t);
        assert(cmd.speed == 0 && cmd.turn == 0 && t.left == 0 && t.right == 0);
        assert(!mode.getTelemetry().stopped);
        // Telemetry F/B/L/R carry each channel's force above the empty plate.
        assert(mode.getTelemetry().front == 647 && mode.getTelemetry().back == 458);
        assert(mode.getTelemetry().left == 230 && mode.getTelemetry().right == 390);

        cmd = drive(mode, Forward, &t);
        assert(near(cmd.speed, 1) && near(cmd.turn, 0.06865f) && near(t.left, 1) && near(t.right, 0.92820f));
        cmd = drive(mode, BackTouch, &t);
        assert(near(cmd.speed, -0.99062f) && cmd.turn == 0 && near(t.left, -0.99062f) && near(t.right, -0.99062f));
        cmd = drive(mode, Right, &t);
        assert(near(cmd.speed, -0.06045f) && near(cmd.turn, 1) && near(t.left, 0.34146f) && near(t.right, -0.46236f));
        cmd = drive(mode, Left, &t);
        assert(cmd.speed == 0 && near(cmd.turn, -1) && near(t.left, -0.33880f) && near(t.right, 0.33880f));
        // Relaxing into passive neutral is 0.74" forward of active neutral: a slow creep.
        cmd = drive(mode, PassiveNeutral, &t);
        assert(near(cmd.speed, 0.13933f) && cmd.turn == 0);

        // An unloaded plate can't be located: stopped, not a wild command.
        cmd = drive(mode, EmptyPlate, &t);
        assert(t.left == 0 && t.right == 0 && mode.getTelemetry().stopped);
        assert(!mode.getCentreOfPressure().valid);
    }

    void seatDetectionIsLayoutIndependent()
    {
        // Total load is Σ(raw − default floor) on both layouts, against the same W.
        const lean::Counts cases[] = {EmptyPlate, ActiveNeutral, BackTouch, {{420, 410, 430, 405}}, {{600, 560, 450, 420}}};
        for (const auto &raw : cases)
        {
            bool seated[2];
            for (auto layout : {SensorLayout::Plus, SensorLayout::Corner})
            {
                setLayout(layout);
                auto mode = freshMode();
                sensors(ActiveNeutral);
                mode.calibrate();
                sensors(raw);
                mode.sampleSensors(/*seatOnly=*/true);
                seated[static_cast<int>(layout)] = mode.isSeated();
            }
            assert(seated[0] == seated[1]);
        }
        setLayout(SensorLayout::Corner);
        auto mode = freshMode();
        sensors(ActiveNeutral);
        mode.calibrate();
        sensors(EmptyPlate);
        mode.sampleSensors(true);
        assert(!mode.isSeated()); // empty-seat check still trips
        sensors(BackTouch);
        mode.sampleSensors(true);
        assert(mode.isSeated()); // leaning back onto the backrest keeps the rider seated
    }

    void layoutSwitch()
    {
        // Calibrated on one layout, a different live layout keeps LEAN stopped
        // until the rider recalibrates (entering LEAN also forces that).
        setLayout(SensorLayout::Corner);
        auto mode = freshMode();
        sensors(ActiveNeutral);
        mode.calibrate();
        MotorTargets t;
        drive(mode, Forward, &t);
        assert(t.left > 0.9f);

        setLayout(SensorLayout::Plus);
        drive(mode, Forward, &t);
        assert(t.left == 0 && t.right == 0 && mode.getTelemetry().stopped);
        assert(!mode.getCentreOfPressure().valid); // no CoP on the plus layout

        // Recalibrated on plus, the plus math runs: (F−B)/W and (R−L)/W with plus gains.
        sensors(ActiveNeutral);
        mode.calibrate();
        auto cmd = drive(mode, ActiveNeutral, &t);
        assert(cmd.speed == 0 && cmd.turn == 0);
        cmd = drive(mode, {{1138, 864, 657, 790}}, &t);
        const auto plus = lean::shapeCommand(
            lean::plusCommand(static_cast<int>(100 * settings::frontGain.get()), 0, mode.getUserWeightEstimate()),
            settings::deadband.get(), std::log(0.09f) / std::log(0.21f), std::log(0.20f) / std::log(0.35f));
        assert(near(cmd.speed, plus.speed) && cmd.turn == 0);
        assert(mode.getTelemetry().front == static_cast<int>(100 * settings::frontGain.get()));

        setLayout(SensorLayout::Corner);
        drive(mode, Forward, &t);
        assert(t.left == 0 && t.right == 0); // stale plus calibration
        sensors(ActiveNeutral);
        mode.calibrate();
        drive(mode, Forward, &t);
        assert(t.left > 0.9f);
    }

    void dynamicReach()
    {
        setLayout(SensorLayout::Corner);
        auto mode = freshMode();
        const auto configured = mode.activeReach();
        assert(configured.forward == lean::DefaultReachForward && configured.back == lean::DefaultReachBack);
        assert(configured.left == lean::DefaultReachLeft && configured.right == lean::DefaultReachRight);

        // Edits apply live while no rider reach is calibrated.
        set(settings::reachForward, 3.0f);
        assert(mode.activeReach().forward == 3.0f);
        settings::reachForward.reset();

        // Hold the button in active neutral, lean all four ways, release.
        sensors(ActiveNeutral);
        mode.beginCalibration(1000);
        auto step = [&](const lean::Counts &raw, uint32_t now, bool held = true) {
            sensors(raw);
            mode.sampleSensors();
            return mode.updateCalibration(now, held);
        };
        step(Forward, 1299); // before the 300 ms hold: ignored
        for (const auto &raw : {Right, Forward, Left, BackTouch, ActiveNeutral})
            step(raw, 1400);
        assert(mode.getTelemetry().stopped);
        assert(mode.getTargetsFromSample().left == 0);
        assert(step(ActiveNeutral, 1500, false));
        const auto reach = mode.activeReach();
        // Each direction's furthest travel from neutral (CoP at hold averages).
        assert(near(reach.forward, 0.4265f + 1.6349f, 0.001f));
        assert(near(reach.back, 3.5248f - 1.6349f, 0.001f));
        assert(near(reach.left, 2.0289f - 0.2976f, 0.001f));
        assert(near(reach.right, 1.3047f + 0.2976f, 0.001f));
        assert(messages.find("Reach calibration complete: forward 2.06 back 1.89 left 1.73 right 1.60 in.") != std::string::npos);
        // The plus gains are untouched by a corner calibration.
        assert(mode.getDynamicGains()[0] == settings::frontGain.get());

        // Full lean now means this rider's own furthest lean.
        MotorTargets t;
        auto cmd = drive(mode, Forward, &t);
        assert(near(cmd.speed, 1));
        cmd = drive(mode, Right, &t);
        assert(near(cmd.turn, 1));

        // One weak direction rejects all four and keeps the previous reach.
        sensors(ActiveNeutral);
        mode.beginCalibration(2000);
        for (const auto &raw : {Forward, BackTouch, Left})
            step(raw, 2400);
        assert(step(ActiveNeutral, 2500, false));
        assert(messages.find("Reach calibration rejected: right reached 0.49 in, required 1.20.") != std::string::npos);
        assert(mode.activeReach().right == reach.right && mode.activeReach().forward == reach.forward);

        // Recalibrating neutral keeps the rider's reach; a profile change drops it.
        sensors(ActiveNeutral);
        mode.calibrate();
        assert(mode.activeReach().forward == reach.forward);
        mode.reset();
        assert(mode.activeReach().forward == reach.forward);
        mode.resetDynamicGains();
        assert(mode.activeReach().forward == lean::DefaultReachForward);
    }

    void telemetryLines()
    {
        setLayout(SensorLayout::Corner);
        auto mode = freshMode();
        messages.clear();
        sensors(Forward);
        mode.sampleSensors();
        mode.publishCentreOfPressure(1234);
        assert(messages == "DBG COP 1234 0.19 0.43 nan nan 2507\n"); // no neutral yet
        sensors(ActiveNeutral);
        mode.calibrate();
        sensors(Forward);
        mode.sampleSensors();
        messages.clear();
        mode.publishCentreOfPressure(1284);
        assert(messages == "DBG COP 1284 0.19 0.43 0.49 2.06 2507\n");
        sensors(EmptyPlate);
        mode.sampleSensors();
        messages.clear();
        mode.publishCentreOfPressure(1334);
        assert(messages == "DBG COP 1334 nan nan nan nan 0\n");

        sensors(Forward);
        mode.sampleSensors();
        messages.clear();
        mode.publishSensorStatus();
        assert(messages.find("DBG SENSORS raw=927/756/1058/1390 ") == 0);
        assert(messages.find(" railed=0 layout=CORNER\n") != std::string::npos);
        messages.clear();
        mode.publishCornerStatus();
        assert(messages == "DBG CORNER neutral=-0.30/-1.63 passive=nan/nan dead=-0.55/-0.05/-1.88/-1.38 "
                           "full=-2.00/1.30/-3.53/0.37 reachCal=0 empty=391/406/427/400 auto=0\n");
        messages.clear();
        mode.publishDynamicGains(); // the dashboard shows the active layout's gains
        assert(messages == "DYNAMIC_GAINS F=1.0000 B=1.0000 L=1.0000 R=1.0000\n");

        // Worst case for the 256-byte line limit: every number at its widest.
        sensors({{4095, 4095, 4095, 4095}});
        mode.sampleSensors();
        messages.clear();
        mode.publishSensorStatus(); // logf asserts the line fits
        mode.publishCornerStatus();

        // Before any calibration the zone is unknown.
        auto fresh = freshMode();
        messages.clear();
        fresh.publishCornerStatus();
        assert(messages == "DBG CORNER neutral=nan/nan passive=nan/nan dead=nan/nan/nan/nan "
                           "full=nan/nan/nan/nan reachCal=0 empty=391/406/427/400 auto=0\n");

        setLayout(SensorLayout::Plus);
        messages.clear();
        mode.publishCentreOfPressure(1384);
        mode.publishCornerStatus();
        assert(messages.empty()); // no DBG COP or DBG CORNER on the plus layout
        mode.publishSensorStatus();
        assert(messages.find(" layout=PLUS\n") != std::string::npos);
        setLayout(SensorLayout::Corner);
    }
} // namespace

int main()
{
    assert(settings::sensorLayout.get() == SensorLayout::Corner); // the chair's layout since 2026-11-22
    centreOfPressureMath();
    mappingMath();
    e9Holds();
    seatDetectionIsLayoutIndependent();
    layoutSwitch();
    dynamicReach();
    telemetryLines();
    puts("Corner layout CoP, calibration, mapping and layout switch tests passed");
}
