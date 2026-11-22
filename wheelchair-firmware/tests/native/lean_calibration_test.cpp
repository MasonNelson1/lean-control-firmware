// Neutral capture, range calibration and tare on the actual LeanDriveMode.
#include "lean_test_support.h"

using Event = LeanDriveMode::Event;

static void ranges(int f, int b, int l, int r)
{
    assert(settings::rangeForward.set(f) && settings::rangeBack.set(b));
    assert(settings::rangeLeft.set(l) && settings::rangeRight.set(r));
}
static bool rangesAre(int f, int b, int l, int r)
{
    return settings::rangeForward.get() == f && settings::rangeBack.get() == b &&
           settings::rangeLeft.get() == l && settings::rangeRight.get() == r;
}
// Long press, release, still second: leaves the rig collecting ranges.
static void startRange(LeanRig &rig)
{
    rig.mode.beginCalibration(rig.now);
    assert(rig.calibrate(LeanDriveMode::RangeHoldMs + 100, /*held=*/true) == Event::None);
    assert(rig.calibrate(LeanDriveMode::SettleMs + LeanDriveMode::StillWindowMs + 100) == Event::None);
    assert(rig.mode.calibrationActive() && !rig.mode.calibrated());
    assert(logged("Range calibration started"));
}

int main()
{
    { // Neutral is taken after the button is released, over one still second.
        LeanRig rig;
        // The rider is reaching for the button: leaning while it is pressed.
        lean(150, -200);
        rig.sample(300);
        rig.mode.beginCalibration(rig.now);
        assert(rig.mode.calibrationActive() && rig.mode.command().stopped);
        assert(rig.calibrate(200, /*held=*/true) == Event::None);
        assert(rig.mode.calibrationLEDOn(rig.now)); // solid while held
        // Released, hand coming back: still inside the settle time.
        assert(rig.calibrate(LeanDriveMode::SettleMs - 10) == Event::None);
        lean(0, 0);
        // Not done before a full still second has passed...
        assert(rig.calibrate(LeanDriveMode::StillWindowMs) == Event::None);
        // ...but the filter needed a moment to settle, so it restarts once and then completes.
        assert(rig.calibrate(1500) == Event::NeutralDone);
        assert(rig.mode.calibrated() && !rig.mode.calibrationActive());
        // Neutral is the sitting posture, not the reach toward the button.
        auto command = rig.settle();
        assert(!command.stopped && command.speed == 0 && command.turn == 0);
        lean(150, -200);
        command = rig.settle();
        assert(command.speed > 0.05f && command.turn < -0.05f);
    }
    { // A rider who keeps moving never gets a neutral; the chair stays stopped.
        LeanRig rig;
        lean(0, 0);
        rig.sample(300);
        rig.mode.beginCalibration(rig.now);
        rig.calibrate(50, true);
        Event event = Event::None;
        const uint32_t start = rig.now;
        for (int k = 0; event == Event::None && k < 100; ++k)
        { // rocking 80 counts forward and back every 200 ms: more than 10% of the range
            lean(k % 2 ? 80 : 0, 0);
            event = rig.calibrate(200);
        }
        assert(event == Event::NeutralFailed);
        const uint32_t took = rig.now - start;
        assert(took >= LeanDriveMode::NeutralTimeoutMs && took < LeanDriveMode::NeutralTimeoutMs + 800);
        assert(!rig.mode.calibrated() && !rig.mode.calibrationActive());
        assert(rig.mode.command().stopped && rig.mode.calibrationLEDOn(rig.now));
        assert(logged("Calibration failed: not still"));
        // Small movement (breathing) is still: 20 counts on a 400-count range.
        rig.mode.beginCalibration(rig.now);
        rig.calibrate(50, true);
        event = Event::None;
        for (int k = 0; event == Event::None && k < 100; ++k)
        {
            lean(k % 2 ? 20 : 0, k % 2 ? -15 : 0);
            event = rig.calibrate(200);
        }
        assert(event == Event::NeutralDone);
    }
    { // The stillness allowance follows the rider's ranges.
        LeanRig rig;
        ranges(80, 80, 500, 500); // a rider with a small forward/back signal: 8 counts allowed
        lean(0, 0);
        rig.sample(300);
        rig.mode.beginCalibration(rig.now);
        rig.calibrate(50, true);
        Event event = Event::None;
        for (int k = 0; event == Event::None && k < 100; ++k)
        {
            lean(k % 2 ? 20 : 0, 0);
            event = rig.calibrate(200);
        }
        assert(event == Event::NeutralFailed);
        ranges(600, 400, 500, 500);
    }
    { // Range calibration: hold one second, sit still, lean each way, press again.
        LeanRig rig;
        messages.clear();
        lean(0, 0);
        rig.sample(300);
        rig.mode.beginCalibration(rig.now);
        assert(rig.calibrate(LeanDriveMode::RangeHoldMs - 10, true) == Event::None);
        assert(!logged("Range calibration"));
        assert(rig.calibrate(20, true) == Event::None);
        assert(logged("Range calibration: let go"));
        assert(rig.calibrate(LeanDriveMode::SettleMs + LeanDriveMode::StillWindowMs + 100) == Event::None);
        assert(rig.mode.calibrationActive() && rig.mode.command().stopped); // motors stay off throughout
        lean(700, 0);
        rig.calibrate(600);
        lean(0, 0);
        rig.calibrate(300);
        lean(-380, 0);
        rig.calibrate(600);
        lean(0, -450);
        rig.calibrate(600);
        lean(0, 520);
        rig.calibrate(600);
        lean(0, 0);
        rig.calibrate(300);
        assert(rig.calibrate(50, false, /*pressed=*/true) == Event::RangeDone);
        // Each range is 90% of the measured reach.
        assert(rangesAre(630, 342, 405, 468));
        assert(logged("LEAN_RANGES F=630 B=342 L=405 R=468"));
        assert(logged("4 of 4 directions updated"));
        // The rider may still be leaning: neutral has to be captured again.
        assert(!rig.mode.calibrationActive() && !rig.mode.calibrated() && rig.mode.command().stopped);
        // With the new ranges the measured full lean is (just past) full command.
        rig.captureNeutral();
        lean(700, 0);
        assert(std::fabs(rig.settle().speed - 1.0f) < 0.001f);
        lean(315, 0);
        const float half = rig.settle().speed;
        assert(half > 0.2f && half < 0.5f);
    }
    { // Only the directions that were leaned are updated; the rest are kept.
        LeanRig rig;
        ranges(600, 400, 500, 500);
        messages.clear();
        lean(0, 0);
        rig.sample(300);
        startRange(rig);
        lean(300, 0);
        rig.calibrate(600);
        lean(0, 30); // right: only 30 counts, below the 40-count minimum range
        rig.calibrate(600);
        lean(0, 0);
        assert(rig.calibrate(50, false, true) == Event::RangeDone);
        assert(rangesAre(270, 400, 500, 500));
        assert(logged("right lean only reached 30 counts; keeping 500"));
        assert(logged("1 of 4 directions updated"));
    }
    { // Nothing leaned: rejected, ranges untouched. A huge reading is capped.
        LeanRig rig;
        messages.clear();
        lean(0, 0);
        rig.sample(300);
        startRange(rig);
        rig.calibrate(500);
        assert(rig.calibrate(50, false, true) == Event::RangeRejected);
        assert(rangesAre(270, 400, 500, 500) && logged("Range calibration rejected"));
        assert(!rig.mode.calibrated());
        startRange(rig);
        lean(3600, 0);
        rig.calibrate(600);
        assert(rig.calibrate(50, false, true) == Event::RangeDone);
        assert(settings::rangeForward.get() == settings::LeanRangeMax);
    }
    { // No second press: the collection ends by itself after the timeout.
        LeanRig rig;
        ranges(600, 400, 500, 500);
        lean(0, 0);
        rig.sample(300);
        startRange(rig);
        lean(-200, 0);
        rig.calibrate(500);
        const uint32_t start = rig.now;
        assert(rig.calibrate(LeanDriveMode::RangeTimeoutMs + 1000) == Event::RangeDone);
        assert(rig.now - start <= LeanDriveMode::RangeTimeoutMs);
        assert(rangesAre(600, 180, 500, 500));
    }
    { // Cancelling (seat lost, DISARM pressed) keeps the previous ranges.
        LeanRig rig;
        ranges(600, 400, 500, 500);
        messages.clear();
        lean(0, 0);
        rig.sample(300);
        startRange(rig);
        lean(900, 900);
        rig.calibrate(500);
        rig.mode.cancelCalibration();
        assert(!rig.mode.calibrationActive() && !rig.mode.calibrated());
        assert(rig.calibrate(100, false, true) == Event::None);
        assert(rangesAre(600, 400, 500, 500) && logged("Range calibration cancelled"));
    }
    { // LED: solid when LEAN needs calibrating, slow blink while capturing neutral,
      // fast blink once the hold is long enough and while collecting ranges.
        LeanRig rig;
        lean(0, 0);
        rig.sample(300);
        assert(rig.mode.calibrationLEDOn(rig.now));
        rig.mode.beginCalibration(rig.now);
        const uint32_t pressed = rig.now;
        rig.calibrate(900, true);
        assert(rig.mode.calibrationLEDOn(pressed + 50) && rig.mode.calibrationLEDOn(pressed + 150));
        rig.calibrate(200, true); // past one second: fast blink
        assert(rig.mode.calibrationLEDOn(pressed + 1200) && !rig.mode.calibrationLEDOn(pressed + 1300));
        rig.calibrate(LeanDriveMode::SettleMs + 100); // neutral: slow blink
        assert(rig.mode.calibrationLEDOn(pressed + 2000) && !rig.mode.calibrationLEDOn(pressed + 2250));
        rig.calibrate(LeanDriveMode::StillWindowMs + 100); // collecting: fast blink
        assert(rig.mode.calibrationLEDOn(pressed + 4000) && !rig.mode.calibrationLEDOn(pressed + 4100));
        rig.mode.cancelCalibration();
        // Timing survives millis() rollover.
        LeanRig late;
        late.now = UINT32_MAX - 400;
        lean(0, 0);
        late.sample(300);
        late.captureNeutral();
        assert(late.now < 5000 && late.mode.calibrated());
    }
    { // Tare: seat empty, hold two seconds, sensor zeros become the resting readings.
        LeanRig rig;
        messages.clear();
        rawSensors(372, 437, 380, 365); // as logged: back sensor rests higher
        rig.sample(500);
        assert(rig.mode.totalLoad() == 65 + 8 - 7);
        // Released early: nothing happens.
        rig.mode.beginTare(rig.now);
        assert(rig.mode.tareActive() && !rig.mode.calibrationActive());
        rig.sample(500);
        assert(rig.mode.updateTare(rig.now, /*held=*/false) == Event::None && !rig.mode.tareActive());
        // Held: the capture starts after two seconds and takes one still second.
        rig.mode.beginTare(rig.now);
        const uint32_t pressed = rig.now;
        Event event = Event::None;
        while (event == Event::None && rig.now - pressed < 10000)
        {
            rig.sample();
            event = rig.mode.updateTare(rig.now, rig.now - pressed < 2500);
        }
        assert(event == Event::TareDone);
        const uint32_t took = rig.now - pressed;
        assert(took >= LeanDriveMode::TareHoldMs + LeanDriveMode::StillWindowMs && took < 3200);
        assert(settings::zeroFront.get() == 372 && settings::zeroBack.get() == 437);
        assert(settings::zeroLeft.get() == 380 && settings::zeroRight.get() == 365);
        assert(logged("Tare complete: zero F=372 B=437 L=380 R=365"));
        rig.sample(300);
        assert(rig.mode.totalLoad() == 0 && !rig.mode.isSeated());

        // Readings that keep moving (someone on the seat) are rejected after 5 s.
        rig.mode.beginTare(rig.now);
        event = Event::None;
        const uint32_t start = rig.now;
        for (int k = 0; event == Event::None && k < 4000; ++k)
        {
            rawSensors(372 + (k / 20 % 2) * 60, 437, 380, 365);
            rig.sample();
            event = rig.mode.updateTare(rig.now, true);
        }
        assert(event == Event::TareRejected && logged("Tare rejected: the readings kept moving"));
        assert(rig.now - start < LeanDriveMode::TareHoldMs + LeanDriveMode::TareTimeoutMs + 500);
        assert(settings::zeroFront.get() == 372);

        // A reading outside the plausible band (unplugged sensor) is rejected.
        rawSensors(372, 437, 20, 365);
        rig.mode.beginTare(rig.now);
        event = Event::None;
        for (int k = 0; event == Event::None && k < 2000; ++k)
        {
            rig.sample();
            event = rig.mode.updateTare(rig.now, true);
        }
        assert(event == Event::TareRejected && logged("must each be 100 to 1500"));
        assert(settings::zeroLeft.get() == 380);
    }
    puts("Neutral capture, range calibration and tare tests passed");
}
