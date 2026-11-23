#include "LeanDriveMode.h"
#include "Arduino.h"
#include "common.h"
#include "../config/DriveSettings.h"
#include "../telemetry/Telemetry.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

static int aboveFloor(int raw, int floor) { return raw - floor; }

static bool atRail(int raw) { return raw <= LeanDriveMode::SensorRailLow || raw >= LeanDriveMode::SensorRailHigh; }

lean::SensorLayout LeanDriveMode::layout() { return settings::sensorLayout.get(); }

static lean::Counts configuredEmptyPlate()
{
    return {{settings::emptyPlateF.get(), settings::emptyPlateB.get(),
             settings::emptyPlateL.get(), settings::emptyPlateR.get()}};
}

lean::Counts LeanDriveMode::emptyPlate() const { return emptyTracker.baseline(configuredEmptyPlate()); }

static lean::Gains cornerGains()
{
    return {{settings::cornerGainF.get(), settings::cornerGainB.get(),
             settings::cornerGainL.get(), settings::cornerGainR.get()}};
}

lean::CentreOfPressure LeanDriveMode::cornerCop(const lean::Counts &raw) const
{
    return lean::centreOfPressure(lean::cornerForces(raw, emptyPlate(), cornerGains()));
}

// Average of `samples` reads per channel 2 ms apart, rounded half up.
static lean::Counts averageRaw(int samples)
{
    long sumF = 0, sumB = 0, sumL = 0, sumR = 0;
    for (int i = 0; i < samples; i++)
    {
        sumF += analogRead(common::PIN_F);
        sumB += analogRead(common::PIN_B);
        sumL += analogRead(common::PIN_L);
        sumR += analogRead(common::PIN_R);
        delay(2);
    }
    return {{static_cast<int>((sumF + samples / 2) / samples), static_cast<int>((sumB + samples / 2) / samples),
             static_cast<int>((sumL + samples / 2) / samples), static_cast<int>((sumR + samples / 2) / samples)}};
}

lean::NeutralZone LeanDriveMode::neutralZone() const
{
    return lean::neutralZone(neutral.at, passive.valid ? &passive.at : nullptr,
                             settings::cornerDeadzone.get(), settings::passiveMargin.get());
}

bool LeanDriveMode::cornerCalibrated() const
{
    return initiallyCalibrated && calibratedLayout == lean::SensorLayout::Corner && neutral.valid;
}

// Calibrated for the live layout. On the corner layout that also needs a usable
// neutral (enough load, every channel carrying some of it).
bool LeanDriveMode::calibrated() const
{
    if (!initiallyCalibrated || layout() != calibratedLayout)
        return false;
    return calibratedLayout != lean::SensorLayout::Corner || neutral.valid;
}

bool LeanDriveMode::readyToDrive() const
{
    return calibrated() && !calibrationActive();
}

static lean::Reach configuredReach()
{
    return {settings::reachForward.get(), settings::reachBack.get(),
            settings::reachLeft.get(), settings::reachRight.get()};
}

// A hold-calibrated reach stands until a reach setting changes, the seat has
// been empty for ForgetReachEmptyMs (a new rider), or the profile changes.
bool LeanDriveMode::reachFromCalibration() const
{
    return reachCalibrated && configuredReach() == reachConfigAtCalibration;
}

void LeanDriveMode::settingsChanged()
{
    if (reachCalibrated && !(configuredReach() == reachConfigAtCalibration))
    {
        reachCalibrated = false;
        telemetry::Telemetry::logf("Reach setting changed: hold-calibrated reach cleared.\n");
    }
}

lean::Reach LeanDriveMode::activeReach() const
{
    return reachFromCalibration() ? calibratedReach : configuredReach();
}

void LeanDriveMode::updateResponseCurve()
{
    const auto &speedKnee = settings::leanForewardBackResponse.get();
    speedResponseExponent = logf(speedKnee[1]) / logf(speedKnee[0]);
    const auto &turnKnee = settings::leanSideToSideResponse.get();
    turnResponseExponent = logf(turnKnee[1]) / logf(turnKnee[0]);
}

void LeanDriveMode::sampleSensors(bool seatOnly)
{
    const int rawF = analogRead(common::PIN_F);
    const int rawB = analogRead(common::PIN_B);
    const int rawL = analogRead(common::PIN_L);
    const int rawR = analogRead(common::PIN_R);
    railed = atRail(rawF) || atRail(rawB) || atRail(rawL) || atRail(rawR);
    rawReadings = {{rawF, rawB, rawL, rawR}};
    const int defaultFloor = settings::defaultFloor.get();
    seatLoad = (rawF - defaultFloor) + (rawB - defaultFloor) +
               (rawL - defaultFloor) + (rawR - defaultFloor);
    if (seatOnly)
        return;
    if (layout() == lean::SensorLayout::Corner)
    {
        sampleCorner();
        return;
    }
    cop = {};

    int f = aboveFloor(rawF, initiallyCalibrated ? floorF : defaultFloor);
    int b = aboveFloor(rawB, initiallyCalibrated ? floorB : defaultFloor);
    int l = aboveFloor(rawL, initiallyCalibrated ? floorL : defaultFloor);
    int r = aboveFloor(rawR, initiallyCalibrated ? floorR : defaultFloor);

    flooredReadings = {{f, b, l, r}};
    f = static_cast<int>(f * dynamicGains[0]);
    b = static_cast<int>(b * dynamicGains[1]);
    l = static_cast<int>(l * dynamicGains[2]);
    r = static_cast<int>(r * dynamicGains[3]);

    telemetry = {f, b, l, r, 0.0f, 0.0f, !readyToDrive()};
}

void LeanDriveMode::sampleCorner()
{
    const auto forces = lean::cornerForces(rawReadings, emptyPlate(), cornerGains());
    cop = lean::centreOfPressure(forces);
    // The telemetry F/B/L/R columns carry each channel's gain-scaled force above
    // the empty plate, the same numbers the centre of pressure is built from.
    telemetry = {static_cast<int>(forces[lean::F] + 0.5f), static_cast<int>(forces[lean::B] + 0.5f),
                 static_cast<int>(forces[lean::L] + 0.5f), static_cast<int>(forces[lean::R] + 0.5f),
                 0.0f, 0.0f, !readyToDrive()};
}

MotorTargets LeanDriveMode::getTargets()
{
    sampleSensors();
    return getTargetsFromSample();
}

MotorTargets LeanDriveMode::getTargetsFromSample()
{
    copJumped = false;
    if (!readyToDrive())
    {
        lastDrivenCop = {}; // the jump check only compares consecutive driving ticks
        return {0, 0};
    }

    const int f = telemetry.front, b = telemetry.back;
    const int l = telemetry.left, r = telemetry.right;

    // Each layout produces speed and turn from -1 to 1; shapeCommand then applies
    // the shared deadband, response curves and reverse-turn flip.
    lean::Command command;
    if (calibratedLayout == lean::SensorLayout::Corner)
    {
        // A sensor dropping out moves the CoP further in one tick than a body
        // can (lean::MaxCopStep). main.cpp disarms on copJumped.
        copJumped = cop.valid && lastDrivenCop.valid &&
                    hypotf(cop.at.x - lastDrivenCop.at.x, cop.at.y - lastDrivenCop.at.y) > lean::MaxCopStep;
        lastDrivenCop = cop;
        if (!cop.valid || copJumped)
        {
            telemetry.speed = telemetry.turn = 0.0f;
            telemetry.stopped = true;
            return {0, 0};
        }
        // The corner deadzone is in inches inside cornerCommand, with the output
        // ramping from its edge, so the normalised deadband is skipped (0).
        const auto raw = lean::cornerCommand(cop.at, neutralZone(), activeReach());
        // Shape first with no flip, then apply the reverse-turn flip with hysteresis
        // on the SHAPED speed (court 2026-11-22). A single threshold at -0.1 chattered
        // while turning on the spot with a slight back lean; and the raw speed is the
        // wrong signal: a pure full side lean drifts the CoP back to raw -0.16
        // (shaped ~-0.06), which must not count as reversing. Flip once clearly
        // reversing; un-flip only near standstill AND with the turn near neutral.
        command = lean::shapeCommand(raw, 0.0f, speedResponseExponent, turnResponseExponent, -1.0e9f);
        if (!lean::CornerReverseFlipEnabled)
            reverseFlipped = false;
        else if (!reverseFlipped && command.speed < lean::CornerReverseEngage)
            reverseFlipped = true;
        else if (reverseFlipped && command.speed > lean::CornerReverseRelease &&
                 fabsf(command.turn) < lean::CornerReverseReleaseTurn)
            reverseFlipped = false;
        if (reverseFlipped)
            command.turn = -command.turn;
    }
    else
    {
        command = lean::shapeCommand(lean::plusCommand(f - b, r - l, userWeightEstimate),
                                     settings::deadband.get(), speedResponseExponent, turnResponseExponent);
    }
    const float speed = command.speed, turn = command.turn;

    telemetry = {f, b, l, r, speed, turn, false};
    const auto &mix = settings::leanTurnMix.get();
    const float speedMagnitude = fabsf(speed);
    const int segment = speedMagnitude < mix[2] ? 0 : 2;
    const float ratio = (speedMagnitude - mix[segment]) / (mix[segment + 2] - mix[segment]);
    const float turnGain = mix[segment + 1] + ratio * (mix[segment + 3] - mix[segment + 1]);
    float mixedTurn = turnGain * turn;
    // Corner layout: turning is snappy at low speed and tame at speed (court
    // 2026-11-22: reversing at half speed with a full side lean put one wheel at
    // full reverse and the chair into a spin it couldn't brake out of). The turn
    // authority fades from TurnAuthorityStopped to TurnAuthorityFast by
    // TurnAuthorityFadeSpeed, forward or reverse. Plus layout unchanged.
    if (settings::sensorLayout.get() == lean::SensorLayout::Corner)
    {
        const float fade = std::min(speedMagnitude / lean::TurnAuthorityFadeSpeed, 1.0f);
        const float authority = lean::TurnAuthorityStopped + fade * (lean::TurnAuthorityFast - lean::TurnAuthorityStopped);
        // Turning must feel the same on every speed-knob position (Charles, court
        // 2026-11-22): the wheel targets are fractions of the knob's speed cap, so
        // scale the turn by (reference speed / knob speed). Per-wheel clamping to
        // +-1 still applies, which only bites on the lowest knob at speed.
        mixedTurn = constrain(turn, -1.0f, 1.0f) * authority * turnKnobScale;
    }
    return {constrain(speed + mixedTurn, -1.0f, 1.0f),
            constrain(speed - mixedTurn, -1.0f, 1.0f)};
}

void LeanDriveMode::calibrate()
{

    const int samples = settings::calibrationSamples.get();
    const auto average = averageRaw(samples);
    floorF = average[lean::F];
    floorB = average[lean::B];
    floorL = average[lean::L];
    floorR = average[lean::R];
    userWeightEstimate = (floorF + floorB + floorL + floorR) / 4; // integer division ok here.

    initiallyCalibrated = true;
    calibratedLayout = layout();

    telemetry::Telemetry::logf("** Calibrated (%d samples): floor F=%d B=%d L=%d R=%d **\n",
                               samples, floorF, floorB, floorL, floorR);
    neutral = {};
    passive = {}; // a new neutral starts without a relaxed posture
    if (calibratedLayout != lean::SensorLayout::Corner)
        return;
    // The same averaged readings set the rider's neutral centre of pressure. The
    // rider should sit upright and engaged (active neutral) when pressing.
    neutral = cornerCop(average);
    if (!neutral.valid)
    {
        telemetry::Telemetry::logf("Calibration rejected: plate load %.0f is too low for a centre of pressure; LEAN stays stopped.\n",
                                   neutral.total);
        return;
    }
    // Sitting still, every corner carries part of the rider. One that doesn't is
    // a dead or unplugged sensor, and steering from it would be wrong.
    for (size_t i = 0; i < lean::Channels; ++i)
        if (average[i] < lean::DeadChannelRawMax)
        {
            telemetry::Telemetry::logf("Calibration rejected: channel %c (%s) reads raw %d (dead or unplugged reads about 372); check that sensor. LEAN stays stopped.\n",
                                       lean::ChannelLetters[i], lean::CornerNames[i], average[i]);
            neutral.valid = false;
        }
    if (neutral.valid)
        telemetry::Telemetry::logf("** Neutral centre of pressure x=%.2f y=%.2f in (load %.0f) **\n",
                                   neutral.at.x, neutral.at.y, neutral.total);
}

bool LeanDriveMode::relaxedPressDue(uint32_t pressedAt) const
{
    return passiveWindowOpen && cornerCalibrated() && layout() == lean::SensorLayout::Corner &&
           pressedAt - neutralPressedAt <= lean::PassiveWindowMs;
}

// Calibrate button, corner layout: the first press (upright and engaged) takes
// neutral. A second press within PassiveWindowMs, made while relaxed, records
// the relaxed posture instead and closes the window; a later press starts over
// with a new neutral. Holding either press also runs the dynamic reach
// calibration. The plus layout ignores the window: every press is neutral.
void LeanDriveMode::beginCalibration(uint32_t pressedAt)
{
    if (relaxedPressDue(pressedAt))
    {
        capturePassive();
        passiveWindowOpen = false;
    }
    else
    {
        calibrate();
        neutralPressedAt = pressedAt;
        passiveWindowOpen = cornerCalibrated();
    }
    calibrationPressedAt = pressedAt;
    lastDrivenCop = {}; // the rider may have moved while the button was down
    calibrationPeaks.fill(0);
    oppositeAtPeak.fill(0);
    reachPeaks = {};
    calibrationState = CalibrationState::Waiting;
}

bool LeanDriveMode::updateCalibration(uint32_t now, bool buttonHeld)
{
    if (!calibrationActive())
        return false;
    if (!buttonHeld)
    {
        const bool dynamicFinished = calibrationState == CalibrationState::Collecting;
        if (dynamicFinished)
            finishDynamicCalibration();
        calibrationState = CalibrationState::Idle;
        return dynamicFinished;
    }
    if (calibrationState == CalibrationState::Waiting && now - calibrationPressedAt >= 300)
    {
        calibrationState = CalibrationState::Collecting;
        collectionStartedAt = now;
        telemetry::Telemetry::logf("Dynamic calibration started; lean in all four directions, then release.\n");
    }
    if (calibrationState == CalibrationState::Collecting && calibratedLayout == lean::SensorLayout::Corner)
    {
        // Furthest centre-of-pressure travel beyond the neutral zone in each
        // direction, measured the way cornerCommand measures reach.
        if (cop.valid && neutral.valid)
        {
            const auto zone = neutralZone();
            reachPeaks.forward = std::max(reachPeaks.forward, cop.at.y - zone.yForward);
            reachPeaks.back = std::max(reachPeaks.back, zone.yBack - cop.at.y);
            reachPeaks.right = std::max(reachPeaks.right, cop.at.x - zone.x0);
            reachPeaks.left = std::max(reachPeaks.left, zone.x0 - cop.at.x);
        }
    }
    else if (calibrationState == CalibrationState::Collecting)
    {
        const size_t opposite[] = {1, 0, 3, 2};
        for (size_t i = 0; i < calibrationPeaks.size(); ++i)
            if (flooredReadings[i] > calibrationPeaks[i])
            {
                calibrationPeaks[i] = flooredReadings[i];
                oppositeAtPeak[i] = flooredReadings[opposite[i]];
            }
    }
    return false;
}

void LeanDriveMode::finishDynamicCalibration()
{
    // A finished hold always ends in DISARM (main.cpp), so no relaxed press follows.
    passiveWindowOpen = false;
    if (calibratedLayout == lean::SensorLayout::Corner)
    {
        finishReachCalibration();
        return;
    }
    const float threshold = 0.1f * userWeightEstimate;
    const char *names[] = {"front", "back", "left", "right"};
    std::array<int, 4> differences{};
    bool valid = userWeightEstimate > 0;
    if (!valid)
        telemetry::Telemetry::logf("Dynamic calibration rejected: weight estimate must be positive.\n");
    for (size_t i = 0; i < calibrationPeaks.size(); ++i)
    {
        differences[i] = calibrationPeaks[i] - oppositeAtPeak[i];
        if (calibrationPeaks[i] < threshold || calibrationPeaks[i] == 0)
        {
            valid = false;
            telemetry::Telemetry::logf("Dynamic calibration rejected: %s maximum %d, required %.1f.\n",
                                       names[i], calibrationPeaks[i], threshold);
        }
        if (differences[i] <= 0)
        {
            valid = false;
            telemetry::Telemetry::logf("Dynamic calibration rejected: %s difference at peak %d must be positive.\n",
                                       names[i], differences[i]);
        }
    }
    if (!valid)
        return;
    for (size_t i = 0; i < dynamicGains.size(); ++i)
        dynamicGains[i] = static_cast<float>(userWeightEstimate) / differences[i];
    telemetry::Telemetry::logf("Dynamic calibration complete: differences at peaks F=%d B=%d L=%d R=%d.\n",
                               differences[0], differences[1], differences[2], differences[3]);
    publishDynamicGains();
}

// Corner counterpart of the plus gains: each direction's furthest travel becomes
// that direction's full lean. As with the plus layout, one weak direction rejects
// all four and the previous reach stays.
void LeanDriveMode::finishReachCalibration()
{
    const float peaks[] = {reachPeaks.forward, reachPeaks.back, reachPeaks.left, reachPeaks.right};
    const char *names[] = {"forward", "back", "left", "right"};
    bool valid = neutral.valid;
    if (!valid)
        telemetry::Telemetry::logf("Reach calibration rejected: no neutral centre of pressure.\n");
    for (size_t i = 0; i < 4; ++i)
        if (peaks[i] < lean::MinReach)
        {
            valid = false;
            telemetry::Telemetry::logf("Reach calibration rejected: %s reached %.2f in, required %.2f.\n",
                                       names[i], peaks[i], lean::MinReach);
        }
    if (!valid)
        return;
    // Full output comes at ReachUseFraction of the rider's calibration peak, and the
    // two turn directions share the smaller side, so turning feels the same both ways
    // (court test 2026-11-22: right reach calibrated 2.7-2.9" vs left 2.3-2.4", and
    // riding never got past 0.71 turn to the right).
    const float f = lean::ReachUseFraction;
    const float side = std::min(peaks[2], peaks[3]) * f;
    calibratedReach = {std::min(peaks[0] * f, lean::MaxReach), std::min(peaks[1] * f, lean::MaxReach),
                       std::min(side, lean::MaxReach), std::min(side, lean::MaxReach)};
    // Charles, court 2026-11-22: e-stops power-cycle the chair, so the reach is
    // written into the reach settings and main.cpp saves them into the active
    // profile. After a power cycle only the short (neutral) press is needed.
    auto setReach = [](config::ConfigField<float> &field, float value) {
        value = std::max(lean::MinReach, std::min(value, lean::MaxReach));
        uint8_t bytes[sizeof(float)];
        memcpy(bytes, &value, sizeof(value));
        field.update(bytes, sizeof(bytes));
    };
    setReach(settings::reachForward, calibratedReach.forward);
    setReach(settings::reachBack, calibratedReach.back);
    setReach(settings::reachLeft, calibratedReach.left);
    setReach(settings::reachRight, calibratedReach.right);
    reachSavePending = true;
    reachCalibrated = true;
    reachConfigAtCalibration = configuredReach();
    telemetry::Telemetry::logf("Reach calibration complete: forward %.2f back %.2f left %.2f right %.2f in (peaks %.2f/%.2f/%.2f/%.2f).\n",
                               calibratedReach.forward, calibratedReach.back, calibratedReach.left, calibratedReach.right,
                               peaks[0], peaks[1], peaks[2], peaks[3]);
}

void LeanDriveMode::capturePassive()
{
    const auto relaxed = cornerCop(averageRaw(settings::calibrationSamples.get()));
    if (!relaxed.valid)
    {
        telemetry::Telemetry::logf("Relaxed posture rejected: plate load %.0f is too low.\n", relaxed.total);
        return;
    }
    const float dx = relaxed.at.x - neutral.at.x, dy = relaxed.at.y - neutral.at.y;
    if (fabsf(dx) > lean::MaxPassiveOffset || fabsf(dy) > lean::MaxPassiveOffset)
    {
        telemetry::Telemetry::logf("Relaxed posture rejected: %+.2f/%+.2f in from neutral is a lean (limit %.2f); keeping neutral only.\n",
                                   dx, dy, lean::MaxPassiveOffset);
        return;
    }
    passive = relaxed;
    const auto dead = lean::deadBox(neutralZone());
    telemetry::Telemetry::logf("** Relaxed posture x=%.2f y=%.2f in (%+.2f in from neutral); no speed from y=%.2f to %.2f **\n",
                               passive.at.x, passive.at.y, dy, dead.back, dead.front);
}

void LeanDriveMode::trackEmptyPlate(uint32_t now, bool disarmed)
{
    // A seat empty for a few seconds means the rider got out: their
    // hold-calibrated reach no longer applies.
    if (isSeated())
        seatEmptySince = now;
    else if (now - seatEmptySince >= lean::ForgetReachEmptyMs && reachCalibrated)
    {
        reachCalibrated = false;
        telemetry::Telemetry::logf("Seat empty: hold-calibrated reach cleared; using the configured reach.\n");
    }

    const auto configured = configuredEmptyPlate();
    const bool eligible = disarmed && layout() == lean::SensorLayout::Corner && !isSeated() && !railed;
    if (!emptyTracker.update(rawReadings, now, eligible, configured))
        return;
    // Re-zeroes every few seconds while the seat stays empty; only report real moves.
    const auto adopted = emptyTracker.baseline(configured);
    bool moved = false;
    for (size_t i = 0; i < lean::Channels; ++i)
        moved = moved || adopted[i] - lastReportedEmpty[i] >= 3 || lastReportedEmpty[i] - adopted[i] >= 3;
    if (!moved)
        return;
    lastReportedEmpty = adopted;
    telemetry::Telemetry::logf("Empty plate re-zeroed: F=%d B=%d L=%d R=%d (configured %d/%d/%d/%d).\n",
                               adopted[0], adopted[1], adopted[2], adopted[3],
                               configured[0], configured[1], configured[2], configured[3]);
}

void LeanDriveMode::cancelCalibration()
{
    if (calibrationActive())
        telemetry::Telemetry::logf("Dynamic calibration cancelled; keeping active gains.\n");
    calibrationState = CalibrationState::Idle;
}

bool LeanDriveMode::calibrationLEDOn(uint32_t now) const
{
    if (calibrationState == CalibrationState::Collecting)
        return ((now - collectionStartedAt) / 100) % 2 == 0;
    // Slow blink while a second press would record the relaxed posture.
    if (passiveWindowOpen && cornerCalibrated() && now - neutralPressedAt <= lean::PassiveWindowMs)
        return ((now - neutralPressedAt) / 500) % 2 == 1;
    // On until a calibration that can drive: a rejected corner neutral stays lit.
    return !calibrated();
}

void LeanDriveMode::resetDynamicGains()
{
    dynamicGains = {{settings::frontGain.get(), settings::rearGain.get(),
                     settings::leftGain.get(), settings::rightGain.get()}};
    reachCalibrated = false;
    publishDynamicGains();
}

void LeanDriveMode::publishDynamicGains() const
{
    // The dashboard shows these next to each channel as its active gain.
    const auto gains = layout() == lean::SensorLayout::Corner ? cornerGains() : dynamicGains;
    telemetry::Telemetry::logf("DYNAMIC_GAINS F=%.4f B=%.4f L=%.4f R=%.4f\n",
                               gains[0], gains[1], gains[2], gains[3]);
}

void LeanDriveMode::publishSensorStatus() const
{
    telemetry::Telemetry::logf("DBG SENSORS raw=%d/%d/%d/%d floors=%d/%d/%d/%d W=%d seatLoad=%d seated=%d calibrated=%d calActive=%d railed=%d layout=%s\n",
                               rawReadings[0], rawReadings[1], rawReadings[2], rawReadings[3],
                               floorF, floorB, floorL, floorR, userWeightEstimate, seatLoad,
                               isSeated() ? 1 : 0, initiallyCalibrated ? 1 : 0, calibrationActive() ? 1 : 0, railed ? 1 : 0,
                               lean::layoutName(layout()));
}

void LeanDriveMode::publishCornerStatus() const
{
    if (layout() != lean::SensorLayout::Corner)
        return;
    // Plate inches. dead and full are {left x, right x, back y, front y}: the box
    // where output is zero and the box where it reaches full. nan until a corner
    // calibration. reachCal = 1 when a dynamic calibration set the reach; empty
    // is the baseline in use (auto = 1 when the empty-seat zero set it).
    const bool zoned = cornerCalibrated();
    const auto zone = neutralZone();
    const auto dead = lean::deadBox(zone);
    const auto full = lean::fullLeanBox(zone, activeReach());
    const auto empty = emptyPlate();
    const auto z = [zoned](float v) { return zoned ? v : NAN; };
    const auto p = [this](float v) { return passive.valid ? v : NAN; };
    telemetry::Telemetry::logf("DBG CORNER neutral=%.2f/%.2f passive=%.2f/%.2f dead=%.2f/%.2f/%.2f/%.2f "
                               "full=%.2f/%.2f/%.2f/%.2f reachCal=%d empty=%d/%d/%d/%d auto=%d\n",
                               z(neutral.at.x), z(neutral.at.y), p(passive.at.x), p(passive.at.y),
                               z(dead.left), z(dead.right), z(dead.back), z(dead.front),
                               z(full.left), z(full.right), z(full.back), z(full.front),
                               reachFromCalibration() ? 1 : 0, empty[0], empty[1], empty[2], empty[3],
                               emptyTracker.adopted(configuredEmptyPlate()) ? 1 : 0);
}

void LeanDriveMode::publishCentreOfPressure(uint32_t now) const
{
    if (layout() != lean::SensorLayout::Corner)
        return;
    // ms, CoP x and y (inches, plate coordinates), offset from the calibrated
    // neutral (nan until calibrated), total load (gain-scaled counts above the
    // empty plate). x and y are nan when the load is too small to locate.
    const bool located = cop.valid;
    const bool offset = located && neutral.valid && initiallyCalibrated && calibratedLayout == lean::SensorLayout::Corner;
    telemetry::Telemetry::logf("DBG COP %lu %.2f %.2f %.2f %.2f %.0f\n", static_cast<unsigned long>(now),
                               located ? cop.at.x : NAN, located ? cop.at.y : NAN,
                               offset ? cop.at.x - neutral.at.x : NAN, offset ? cop.at.y - neutral.at.y : NAN,
                               cop.total);
}
