#include "ModeStateMachine.h"
#include <cassert>
#include <cstdio>
#include <cstdint>

using namespace common;
using SM = ModeStateMachine;

static SM::Inputs seated()
{
    SM::Inputs in;
    in.seated = true;
    return in;
}
static SM::Inputs press(mode button)
{
    auto in = seated();
    auto &b = button == LEAN ? in.lean : button == REMOTE ? in.remote : in.disarm;
    b.edge = b.pressed = true;
    in.leanHeld = button == LEAN;
    return in;
}
static SM::Inputs profilePress(int slot)
{
    auto in = press(LEAN);
    in.pendingProfile = slot;
    in.profileSettled = true;
    return in;
}
static SM::Inputs holdProfile(int slot)
{
    auto in = seated();
    in.leanHeld = in.profileSettled = true;
    in.pendingProfile = slot;
    return in;
}

int main()
{
    { // ISR edge must survive debounce; a debounced event alone cannot switch.
        SM sm;
        auto in = seated();
        in.lean.pressed = true;
        sm.update(in, 10);
        assert(sm.selected() == REMOTE);
        in.lean.pressed = false;
        in.lean.edge = true;
        assert(sm.needsSeatSample(in));
        sm.update(in, 20);
        assert(sm.needsSeatSample(seated()));
        sm.update(press(LEAN), 40);
        assert(sm.selected() == LEAN && sm.current() == REMOTE);
        const auto t = sm.applySeat(true, 40);
        assert(t.changed && t.resetLean && !t.seatDisarm);
        assert(!sm.applySeat(true, 41).changed);
    }
    { // Unseated presses other than DISARM are discarded, including delayed debounce events.
        SM sm;
        auto in = press(LEAN);
        in.seated = false;
        sm.update(in, 10);
        in = seated();
        in.lean.pressed = true;
        sm.update(in, 50);
        assert(sm.selected() == REMOTE);
    }
    { // DISARM works with an empty seat, e.g. an empty chair driving in REMOTE.
        SM sm;
        SM::Inputs in; // unseated
        in.disarm.edge = true;
        sm.update(in, 10);
        in = {};
        in.disarm.pressed = true; // debounced press arrives on a later tick
        sm.update(in, 50);
        assert(sm.selected() == DISARM);
        const auto t = sm.applySeat(false, 50);
        assert(t.changed && !t.seatDisarm && sm.current() == DISARM);
        // The empty seat still cancels the Assist hold.
        assert(!sm.update({}, 1000).resetAssist && sm.selected() == DISARM);
    }
    { // Seat latch retains intent and requests resets once per transition.
        SM sm;
        sm.begin(LEAN);
        assert(sm.requiresSeat());
        auto t = sm.applySeat(false, 100);
        assert(t.changed && t.seatDisarm && !t.resetLean);
        assert(sm.current() == DISARM && sm.selected() == LEAN);
        assert(!sm.applySeat(false, 200).changed);
        t = sm.applySeat(true, 300);
        assert(t.changed && t.resetLean && sm.current() == LEAN);
        sm.begin(REMOTE);
        assert(!sm.requiresSeat() && !sm.applySeat(false, 100).changed);
    }
    { // DISARM wins simultaneous presses; Assist requires the full 500 ms.
        SM sm;
        auto in = press(DISARM);
        in.lean = in.remote = in.disarm;
        sm.update(in, 100);
        assert(sm.selected() == DISARM);
        assert(!sm.update(seated(), 599).resetAssist);
        assert(sm.selected() == DISARM);
        assert(sm.update(seated(), 600).resetAssist);
        assert(sm.selected() == ASSIST);
        sm.applySeat(true, 600);
        assert(sm.assistLEDOn(600) && !sm.assistLEDOn(1100));
        assert(!sm.update(seated(), 1200).resetAssist);
        sm.update(press(DISARM), 1300);
        sm.update(seated(), 2000);
        assert(sm.selected() == DISARM); // cannot reenter from same press
    }
    { // Releasing DISARM or losing the seat cancels Assist hold.
        SM sm;
        sm.update(press(DISARM), 100);
        auto in = seated();
        in.disarm.released = true;
        sm.update(in, 599);
        sm.update(seated(), 600);
        assert(sm.selected() == DISARM);
        sm.update(press(DISARM), 700);
        sm.update({}, 800);
        sm.update(seated(), 1300);
        assert(sm.selected() == DISARM);
    }
    { // Selecting remote discards its previous command once, and cancels hold.
        SM sm;
        sm.begin(LEAN);
        assert(sm.update(press(REMOTE), 10).resetRemote);
        assert(!sm.update(press(REMOTE), 20).resetRemote);
        sm.update(press(DISARM), 30);
        assert(sm.update(press(REMOTE), 40).resetRemote);
        assert(!sm.update(seated(), 1000).resetAssist);
        assert(sm.selected() == REMOTE);
    }
    { // Pending profiles reserve the Lean button even outside DISARM.
        SM sm;
        sm.update(profilePress(2), 100);
        assert(sm.selected() == REMOTE);
        assert(sm.update(holdProfile(2), 2100).activateProfile == SM::NoProfile);
    }
    { // Profile confirmation fires at 2 seconds and only once.
        SM sm;
        sm.begin(DISARM);
        sm.update(profilePress(2), 100);
        assert(sm.update(holdProfile(2), 2099).activateProfile == SM::NoProfile);
        assert(sm.update(holdProfile(2), 2100).activateProfile == 2);
        assert(sm.selected() == DISARM);
        assert(sm.update(holdProfile(2), 5000).activateProfile == SM::NoProfile);
    }
    for (int cancel = 0; cancel < 6; ++cancel)
    { // Raw release, detent change, unstable knob, seat loss, remote, disarm.
        SM sm;
        sm.begin(DISARM);
        sm.update(profilePress(1), 100);
        auto in = holdProfile(1);
        if (cancel == 0) in.leanHeld = false;
        if (cancel == 1) in.pendingProfile = 2;
        if (cancel == 2) in.profileSettled = false;
        if (cancel == 3) in.seated = false;
        if (cancel == 4) in.remote.edge = in.remote.pressed = true;
        if (cancel == 5) in.disarm.edge = in.disarm.pressed = true;
        sm.update(in, 500);
        assert(sm.update(holdProfile(1), 2100).activateProfile == SM::NoProfile);
    }
    { // Calibration requests latch only in selected Lean and drain once.
        SM sm;
        auto in = seated();
        in.calibrate.pressed = true;
        sm.update(in, 10);
        assert(!sm.takeCalibrationRequest());
        sm.begin(LEAN);
        sm.update(in, 20);
        sm.update(seated(), 21);
        assert(sm.takeCalibrationRequest());
        assert(!sm.takeCalibrationRequest());
    }
    { // Assist and profile hold arithmetic survives millis() rollover.
        SM sm;
        const uint32_t start = UINT32_MAX - 100;
        sm.update(press(DISARM), start);
        assert(!sm.update(seated(), start + 499).resetAssist);
        assert(sm.update(seated(), start + 500).resetAssist);
        sm.begin(DISARM);
        sm.update(profilePress(0), start);
        assert(sm.update(holdProfile(0), start + 2000).activateProfile == 0);
    }
    { // A debounced release discards a pending edge.
        SM sm;
        auto in = seated();
        in.lean.edge = true;
        sm.update(in, 10);
        in = seated();
        in.lean.released = true;
        sm.update(in, 20);
        in = seated();
        in.lean.pressed = true;
        sm.update(in, 30);
        assert(sm.selected() == REMOTE);
        assert(!sm.needsSeatSample(seated()));
    }
    { // Calibration discards even partly debounced mode presses and profile holds.
        SM sm;
        sm.begin(LEAN);
        auto in = seated();
        in.remote.edge = true;
        sm.update(in, 10);
        in = press(REMOTE);
        in.calibrationActive = true;
        assert(!sm.update(in, 50).resetRemote);
        in = seated();
        in.remote.pressed = true;
        sm.update(in, 60);
        assert(sm.selected() == LEAN);
        in = press(DISARM);
        in.calibrationActive = true;
        sm.update(in, 70);
        sm.update(seated(), 1000);
        assert(sm.selected() == LEAN);
        const auto seatLoss = sm.applySeat(false, 1001);
        assert(seatLoss.changed && seatLoss.seatDisarm);
        sm.update(press(REMOTE), 1100);
        assert(sm.selected() == REMOTE);
    }
    { // A completed dynamic calibration disarms until a fresh mode selection.
        SM sm;
        sm.begin(LEAN);
        sm.requestDisarm();
        assert(sm.selected() == DISARM);
        const auto transition = sm.applySeat(true, 100);
        assert(transition.changed && !transition.seatDisarm && !transition.resetLean);
        assert(sm.current() == DISARM);
        sm.update(seated(), 1000);
        assert(!sm.applySeat(true, 1000).changed);
        assert(sm.current() == DISARM);
        sm.update(press(LEAN), 1100);
        assert(sm.applySeat(true, 1100).resetLean);
        assert(sm.current() == LEAN);
    }
    { // A programmatic disarm clears an earlier Assist hold.
        SM sm;
        sm.update(press(DISARM), 10);
        sm.requestDisarm();
        assert(!sm.update(seated(), 1000).resetAssist);
        sm.applySeat(true, 1000);
        assert(sm.current() == DISARM);
    }
    puts("Mode state machine regression tests passed");
}
