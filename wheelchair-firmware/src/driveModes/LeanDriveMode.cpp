#include "LeanDriveMode.h"
#include "Arduino.h"
#include "common.h"
#include "../config/DriveSettings.h"
#include "../telemetry/Telemetry.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Raw 12-bit counts. An unloaded sensor reads about 372, so only a shorted or open sensor gets here.
    constexpr int SENSOR_RAIL_LOW = 100, SENSOR_RAIL_HIGH = 4000;
    bool atRail(int raw) { return raw <= SENSOR_RAIL_LOW || raw >= SENSOR_RAIL_HIGH; }

    float clampUnit(float value) { return std::min(1.0f, std::max(-1.0f, value)); }

    // Deadband, then the power curve. The input is rescaled so the output
    // starts from zero at the edge of the deadband instead of stepping up.
    float shape(float input, float deadband, float exponent)
    {
        const float magnitude = std::fabs(input);
        if (magnitude <= deadband)
            return 0.0f;
        return std::copysign(std::pow((magnitude - deadband) / (1.0f - deadband), exponent), input);
    }
} // namespace

void LeanDriveMode::updateResponseCurve()
{
    const auto &speedKnee = settings::leanForewardBackResponse.get();
    speedResponseExponent = logf(speedKnee[1]) / logf(speedKnee[0]);
    const auto &turnKnee = settings::leanSideToSideResponse.get();
    turnResponseExponent = logf(turnKnee[1]) / logf(turnKnee[0]);
}

void LeanDriveMode::sample(float dt)
{
    raw = {{analogRead(common::PIN_F), analogRead(common::PIN_B),
            analogRead(common::PIN_L), analogRead(common::PIN_R)}};
    const int zero[4] = {settings::zeroFront.get(), settings::zeroBack.get(),
                         settings::zeroLeft.get(), settings::zeroRight.get()};

    const bool railed = atRail(raw[0]) || atRail(raw[1]) || atRail(raw[2]) || atRail(raw[3]);
    railCount = railed ? std::min<uint8_t>(railCount + 1, RailSamples) : 0;

    const float alpha = primed ? dt / (FilterS + dt) : 1.0f;
    for (size_t i = 0; i < load.size(); ++i)
        load[i] += alpha * (static_cast<float>(raw[i] - zero[i]) - load[i]);
    primed = true;
    total = load[0] + load[1] + load[2] + load[3];
    leanY = load[0] - load[1];
    leanX = load[3] - load[2];

    // Seated above the threshold; empty again only well below it, and only
    // after it has stayed there, so a bump or a hard lean cannot flicker it.
    // This uses the unfiltered sum: the hold times already reject noise, and
    // the filter's lag would add to the time an empty seat takes to register.
    const float threshold = static_cast<float>(settings::seatThreshold.get());
    const float seatLoad = static_cast<float>(raw[0] + raw[1] + raw[2] + raw[3] - zero[0] - zero[1] - zero[2] - zero[3]);
    const bool wantsChange = seated ? seatLoad < SeatReleaseFraction * threshold : seatLoad >= threshold;
    seatTimer = wantsChange ? seatTimer + dt : 0.0f;
    if (wantsChange && seatTimer >= (seated ? SeatOffS : SeatOnS))
    {
        seated = !seated;
        seatTimer = 0.0f;
    }

    telemetry.front = static_cast<int>(lroundf(load[0]));
    telemetry.back = static_cast<int>(lroundf(load[1]));
    telemetry.left = static_cast<int>(lroundf(load[2]));
    telemetry.right = static_cast<int>(lroundf(load[3]));
    // No lean command exists until command() computes one from this sample
    // (LEAN only). In every other mode the telemetry shows zero, not the last
    // command from when LEAN was active.
    telemetry.speed = telemetry.turn = 0.0f;
    telemetry.stopped = true;
}

void LeanDriveMode::reset()
{
    neutralValid = false;
    cancelCalibration();
}

// Lean beyond neutral on each axis, in counts: forward and right positive.
// Each sensor contributes only the load it has gained since neutral (see the
// header for why lost load is ignored).
void LeanDriveMode::effectiveLean(float &forward, float &right) const
{
    float gained[4];
    for (size_t i = 0; i < 4; ++i)
        gained[i] = std::max(load[i] - neutralLoad[i], 0.0f);
    forward = gained[0] - gained[1];
    right = gained[3] - gained[2];
}

LeanDriveMode::Command LeanDriveMode::command(float travel)
{
    if (!neutralValid || calibrationActive())
        return {0.0f, 0.0f, true};

    float y, x;
    effectiveLean(y, x);
    const float rangeY = static_cast<float>(y >= 0.0f ? settings::rangeForward.get() : settings::rangeBack.get());
    const float rangeX = static_cast<float>(x >= 0.0f ? settings::rangeRight.get() : settings::rangeLeft.get());
    const float deadband = settings::deadband.get();

    float speed = shape(clampUnit(y / rangeY), std::max(deadband, DeadbandMinCounts / rangeY), speedResponseExponent);
    float turn = shape(clampUnit(x / rangeX), std::max(deadband, DeadbandMinCounts / rangeX), turnResponseExponent);

    // Reversing: flip turn so leaning back-left still swings the chair left the way
    // it feels natural. Threshold -0.3, not 0, was picked by feel on the chair; at 0
    // turning near a standstill felt wrong. Below -0.9 (hard reverse) turning is off.
    //
    // Two things changed around the team's numbers, which are still the centre:
    // - The flip used to be a step: with the speed hovering at -0.3 the steering
    //   jumped between full left and full right. It now fades through zero
    //   between -0.2 and -0.4, and the cut-off at -0.9 fades in from -0.8.
    // - A back lean while the chair is still rolling forward is braking, not
    //   reversing. Flipping the turn there steered the chair the wrong way
    //   (lean back and right at speed, turn left). So neither rule applies
    //   while the chair travels forward faster than 15% of the speed limit;
    //   they fade in between 15% and 5% as it slows.
    const float forwardTravel = std::min(1.0f, std::max(0.0f, (travel - 0.05f) / 0.10f));
    const float reverseBlend = std::max(clampUnit((speed + 0.3f) / 0.1f), 2.0f * forwardTravel - 1.0f);
    const float hardReverseFade = std::max(std::min(1.0f, std::max(0.0f, (speed + 0.9f) / 0.1f)), forwardTravel);
    turn *= reverseBlend * hardReverseFade;

    telemetry.speed = speed;
    telemetry.turn = turn;
    telemetry.stopped = false;
    return {speed, turn, false};
}

// ---- Still windows ---------------------------------------------------------

void LeanDriveMode::startWindow(uint32_t now)
{
    windowSince = now;
    windowSamples = 0;
    sumLoad.fill(0.0f);
    minY = maxY = leanY;
    minX = maxX = leanX;
    rawSum.fill(0);
    rawMin = rawMax = raw;
}

bool LeanDriveMode::windowStill(float toleranceY, float toleranceX) const
{
    return maxY - minY <= toleranceY && maxX - minX <= toleranceX;
}

// ---- Neutral and range calibration (LEAN) ------------------------------------

void LeanDriveMode::beginCalibration(uint32_t now)
{
    neutralValid = false;
    rangeRequested = false;
    state = State::Held;
    pressedAt = stateSince = now;
}

LeanDriveMode::Event LeanDriveMode::updateCalibration(uint32_t now, bool buttonHeld, bool buttonPressed)
{
    switch (state)
    {
    case State::Held:
        if (buttonHeld)
        {
            if (!rangeRequested && now - pressedAt >= RangeHoldMs)
            {
                rangeRequested = true;
                telemetry::Telemetry::logf("Range calibration: let go and sit still, then lean fully each way and press again.\n");
            }
            break;
        }
        state = State::Settle;
        stateSince = now;
        break;

    case State::Settle:
        if (now - stateSince < SettleMs)
            break;
        state = State::Neutral;
        captureSince = now;
        startWindow(now);
        break;

    case State::Neutral:
    {
        for (size_t i = 0; i < load.size(); ++i)
            sumLoad[i] += load[i];
        ++windowSamples;
        minY = std::min(minY, leanY);
        maxY = std::max(maxY, leanY);
        minX = std::min(minX, leanX);
        maxX = std::max(maxX, leanX);

        const float toleranceY = std::max<float>(StillMinCounts, StillFraction * std::min(settings::rangeForward.get(), settings::rangeBack.get()));
        const float toleranceX = std::max<float>(StillMinCounts, StillFraction * std::min(settings::rangeLeft.get(), settings::rangeRight.get()));
        if (!windowStill(toleranceY, toleranceX))
        {
            if (now - captureSince >= NeutralTimeoutMs)
            {
                state = State::Idle;
                telemetry::Telemetry::logf("Calibration failed: not still (moved F/B %.0f, L/R %.0f counts; allowed %.0f, %.0f). Press again.\n",
                                           maxY - minY, maxX - minX, toleranceY, toleranceX);
                return Event::NeutralFailed;
            }
            startWindow(now);
            break;
        }
        if (now - windowSince < StillWindowMs)
            break;

        for (size_t i = 0; i < load.size(); ++i)
            neutralLoad[i] = sumLoad[i] / windowSamples;
        neutralTotal = neutralLoad[0] + neutralLoad[1] + neutralLoad[2] + neutralLoad[3];
        telemetry::Telemetry::logf("** Calibrated: neutral loads F=%.0f B=%.0f L=%.0f R=%.0f, total %.0f **\n",
                                   neutralLoad[0], neutralLoad[1], neutralLoad[2], neutralLoad[3], neutralTotal);
        if (!rangeRequested)
        {
            neutralValid = true;
            state = State::Idle;
            return Event::NeutralDone;
        }
        state = State::Range;
        stateSince = now;
        // From here the extremes are of the lean beyond neutral, measured the
        // same way command() measures it.
        minY = maxY = minX = maxX = 0.0f;
        telemetry::Telemetry::logf("Range calibration started: lean fully forward, back, left and right, then press the button.\n");
        break;
    }

    case State::Range:
    {
        float y, x;
        effectiveLean(y, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        if (buttonPressed || now - stateSince >= RangeTimeoutMs)
            return finishRange();
        break;
    }

    default:
        break;
    }
    return Event::None;
}

LeanDriveMode::Event LeanDriveMode::finishRange()
{
    state = State::Idle;
    // The rider may still be leaning: neutral must be captured again before driving.
    neutralValid = false;

    config::ConfigField<int> *fields[] = {&settings::rangeForward, &settings::rangeBack,
                                          &settings::rangeLeft, &settings::rangeRight};
    const float reach[] = {maxY, -minY, -minX, maxX};
    const char *names[] = {"forward", "back", "left", "right"};
    int accepted = 0;
    for (size_t i = 0; i < 4; ++i)
    {
        const int range = static_cast<int>(lroundf(reach[i] * RangeFullFraction));
        if (reach[i] < RangeMinReach)
        {
            telemetry::Telemetry::logf("Range calibration: %s lean only reached %.0f counts (%.0f needed); keeping %d.\n",
                                       names[i], reach[i], RangeMinReach, fields[i]->get());
            continue;
        }
        fields[i]->set(std::min(range, settings::LeanRangeMax));
        ++accepted;
    }
    if (accepted == 0)
    {
        telemetry::Telemetry::logf("Range calibration rejected: no direction was leaned far enough.\n");
        return Event::RangeRejected;
    }
    telemetry::Telemetry::logf("Range calibration complete: %d of 4 directions updated.\n", accepted);
    publishRanges();
    return Event::RangeDone;
}

void LeanDriveMode::cancelCalibration()
{
    if (state == State::Idle)
        return;
    if (state == State::Range)
        telemetry::Telemetry::logf("Range calibration cancelled; keeping the previous ranges.\n");
    state = State::Idle;
}

// ---- Tare (DISARM, seat empty) ---------------------------------------------

void LeanDriveMode::beginTare(uint32_t now)
{
    state = State::TareHeld;
    pressedAt = now;
}

LeanDriveMode::Event LeanDriveMode::updateTare(uint32_t now, bool buttonHeld)
{
    if (state == State::TareHeld)
    {
        if (!buttonHeld)
            state = State::Idle; // released early: nothing happens
        else if (now - pressedAt >= TareHoldMs)
        {
            state = State::TareCapture;
            captureSince = now;
            startWindow(now);
            telemetry::Telemetry::logf("Tare started: keep the seat empty.\n");
        }
        return Event::None;
    }
    if (state != State::TareCapture)
        return Event::None;

    bool still = true;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        rawSum[i] += raw[i];
        rawMin[i] = std::min(rawMin[i], raw[i]);
        rawMax[i] = std::max(rawMax[i], raw[i]);
        still = still && rawMax[i] - rawMin[i] <= TareStillCounts;
    }
    ++windowSamples;
    if (!still)
    {
        if (now - captureSince >= TareTimeoutMs)
        {
            state = State::Idle;
            telemetry::Telemetry::logf("Tare rejected: the readings kept moving.\n");
            return Event::TareRejected;
        }
        startWindow(now);
        return Event::None;
    }
    if (now - windowSince < StillWindowMs)
        return Event::None;
    return finishTare();
}

LeanDriveMode::Event LeanDriveMode::finishTare()
{
    state = State::Idle;
    int zero[4];
    for (size_t i = 0; i < 4; ++i)
    {
        zero[i] = static_cast<int>((rawSum[i] + windowSamples / 2) / windowSamples);
        if (zero[i] < settings::SensorZeroMin || zero[i] > settings::SensorZeroMax)
        {
            telemetry::Telemetry::logf("Tare rejected: readings F=%d B=%d L=%d R=%d must each be %d to %d.\n",
                                       static_cast<int>(rawSum[0] / windowSamples), static_cast<int>(rawSum[1] / windowSamples),
                                       static_cast<int>(rawSum[2] / windowSamples), static_cast<int>(rawSum[3] / windowSamples),
                                       settings::SensorZeroMin, settings::SensorZeroMax);
            return Event::TareRejected;
        }
    }
    settings::zeroFront.set(zero[0]);
    settings::zeroBack.set(zero[1]);
    settings::zeroLeft.set(zero[2]);
    settings::zeroRight.set(zero[3]);
    // Loads are measured from the new zeros: restart the filter and the seat state.
    primed = false;
    seated = false;
    seatTimer = 0.0f;
    neutralValid = false;
    telemetry::Telemetry::logf("** Tare complete: zero F=%d B=%d L=%d R=%d **\n", zero[0], zero[1], zero[2], zero[3]);
    return Event::TareDone;
}

// ---- Status ----------------------------------------------------------------

bool LeanDriveMode::calibrationLEDOn(uint32_t now) const
{
    switch (state)
    {
    case State::Held:
        // Solid until the hold is long enough for the range calibration, then fast.
        return !rangeRequested || ((now - pressedAt) / 100) % 2 == 0;
    case State::Settle:
    case State::Neutral:
        return ((now - pressedAt) / 250) % 2 == 0; // slow: sit still
    case State::Range:
    case State::TareCapture:
        return ((now - pressedAt) / 100) % 2 == 0; // fast: measuring
    case State::TareHeld:
        return true;
    default:
        return !neutralValid; // on: LEAN needs a calibration
    }
}

void LeanDriveMode::publishRanges() const
{
    telemetry::Telemetry::logf("LEAN_RANGES F=%d B=%d L=%d R=%d\n",
                               settings::rangeForward.get(), settings::rangeBack.get(),
                               settings::rangeLeft.get(), settings::rangeRight.get());
}
