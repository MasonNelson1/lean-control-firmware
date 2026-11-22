
#pragma once

#include "DriveMode.h"
#include "../telemetry/TelemetryTypes.h"
#include <array>
#include <cmath>
#include <cstdint>

// Reads the four seat sensors and turns the rider's lean into a speed and a
// turn command, each from -1 to 1.
//
//   load   = reading - that sensor's zero (its reading with the seat empty)
//   lean   = (front - back) and (right - left), minus the same differences
//            captured while the rider sat neutral
//   input  = lean / the full-lean range for that direction (settings, set by
//            the range calibration), limited to -1..1
//   output = deadband, then the response curve
//
// Why differences divided by a fixed range, and not a centre of pressure
// (difference / total load): on this seat the total is not the rider's weight.
// It more than doubles in a lean (run2 log: about 330 counts neutral, over
// 1000 leaning), so dividing by it would make a forward lean read as a turn
// and would tie the feel near neutral to a small, unrepeatable number.
// A difference of an opposite pair cancels anything that loads both sensors
// together, and the range is measured per rider and kept.
//
// The same samples decide whether a rider is seated and whether a sensor has
// failed. Sensor order in arrays is front, back, left, right.
class LeanDriveMode
{
public:
    struct Command
    {
        float speed; // forward +, shaped, -1..1
        float turn;  // right +, shaped, -1..1
        bool stopped; // no usable lean input: not calibrated, or calibrating
    };
    enum class Event : uint8_t
    {
        None,
        NeutralDone,
        NeutralFailed,
        RangeDone,     // at least one full-lean range was measured and set
        RangeRejected, // no direction was leaned far enough; ranges unchanged
        TareDone,      // sensor zeros were measured and set
        TareRejected,
    };

    // Timing and thresholds. All times in milliseconds.
    static constexpr uint32_t RangeHoldMs = 1000;   // hold this long for the range calibration
    static constexpr uint32_t SettleMs = 300;       // after release: time for the hand to come back
    static constexpr uint32_t StillWindowMs = 1000; // neutral is the average of one still second
    static constexpr uint32_t NeutralTimeoutMs = 6000;
    static constexpr uint32_t RangeTimeoutMs = 15000;
    static constexpr uint32_t TareHoldMs = 2000;
    static constexpr uint32_t TareTimeoutMs = 5000;
    // The rider is still when neither lean difference moves more than this
    // share of the smaller range of its axis during the window.
    static constexpr float StillFraction = 0.10f;
    static constexpr int StillMinCounts = 8;
    static constexpr int TareStillCounts = 30;
    // A measured full lean maps slightly past full command, so the rider does
    // not have to strain to the very end of their reach for top speed.
    static constexpr float RangeFullFraction = 0.9f;
    // The deadband is never narrower than this, whatever the range: ADC noise.
    static constexpr float DeadbandMinCounts = 6.0f;
    // Seat: empty again below this share of the threshold, held this long.
    static constexpr float SeatReleaseFraction = 0.6f;
    static constexpr float SeatOnS = 0.30f, SeatOffS = 0.10f;
    // Sensor readings are low-pass filtered with this time constant. The
    // response to a lean stays well inside the 0.1 s of CDR Spec K.
    static constexpr float FilterS = 0.025f;
    // A reading must sit at an ADC rail for this many samples to be a fault.
    static constexpr uint8_t RailSamples = 4;

    // Read the sensors. Call once per control tick in every mode.
    void sample(float dt);
    bool isSeated() const { return seated; }
    // True when a sensor has read at an ADC rail (shorted or open) for RailSamples samples.
    bool sensorAtRail() const { return railCount >= RailSamples; }
    int totalLoad() const { return static_cast<int>(lroundf(total)); }

    // Forget neutral. LEAN will not drive until the rider calibrates again.
    void reset();
    bool calibrated() const { return neutralValid; }
    // The lean command from the last sample.
    Command command();

    // Calibration button, in LEAN. A short press captures neutral once the
    // rider has let go and sat still. Holding for RangeHoldMs also measures
    // the full-lean ranges: neutral first, then lean each way, then press
    // again (or wait for the timeout).
    void beginCalibration(uint32_t now);
    Event updateCalibration(uint32_t now, bool buttonHeld, bool buttonPressed);
    void cancelCalibration();
    bool calibrationActive() const { return state != State::Idle && !tareActive(); }

    // Calibration button, in DISARM with the seat empty: hold for TareHoldMs
    // to measure each sensor's zero.
    void beginTare(uint32_t now);
    Event updateTare(uint32_t now, bool buttonHeld);
    bool tareActive() const { return state == State::TareHeld || state == State::TareCapture; }

    bool calibrationLEDOn(uint32_t now) const;
    void publishRanges() const;
    void updateResponseCurve();
    const telemetry::SensorTelemetry &getTelemetry() const { return telemetry; }
    // Total load captured with the rider neutral; 0 until calibrated.
    int getUserWeightEstimate() const { return neutralValid ? static_cast<int>(lroundf(neutralTotal)) : 0; }

private:
    enum class State : uint8_t { Idle, Held, Settle, Neutral, Range, TareHeld, TareCapture };

    void startWindow(uint32_t now);
    bool windowStill(float toleranceY, float toleranceX) const;
    Event finishRange();
    Event finishTare();

    telemetry::SensorTelemetry telemetry{};
    std::array<int, 4> raw{};
    std::array<float, 4> load{}; // filtered, above each sensor's zero
    bool primed = false;         // filter has its first sample
    float total = 0.0f, leanY = 0.0f, leanX = 0.0f; // front-back, right-left

    bool seated = false;
    float seatTimer = 0.0f;
    uint8_t railCount = 0;

    bool neutralValid = false;
    float neutralY = 0.0f, neutralX = 0.0f, neutralTotal = 0.0f;

    float speedResponseExponent = 2.0f; // default initializer for response curve, should be overwritten before its ever used.
    float turnResponseExponent = 2.0f;

    State state = State::Idle;
    bool rangeRequested = false;
    uint32_t pressedAt = 0, stateSince = 0, captureSince = 0, windowSince = 0;
    // One still-window: sums for the average, extremes for the stillness test.
    uint32_t windowSamples = 0;
    float sumY = 0.0f, sumX = 0.0f, sumTotal = 0.0f;
    float minY = 0.0f, maxY = 0.0f, minX = 0.0f, maxX = 0.0f;
    std::array<long, 4> rawSum{};
    std::array<int, 4> rawMin{}, rawMax{};
};
