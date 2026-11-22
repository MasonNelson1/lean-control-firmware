#pragma once

#include "Mode.h"
#include <cstdint>

// Pure transition logic. The caller samples hardware and applies returned actions.
class ModeStateMachine
{
public:
    static constexpr int NoProfile = -1;
    // After a drive mode is stopped, a DISARM press cannot start the Assist
    // hold for this long: a second press in a panic is still only a stop.
    static constexpr uint32_t AssistArmDelayMs = 1000;
    struct Button
    {
        bool edge = false;
        bool pressed = false;
        bool released = false;
    };
    struct Inputs
    {
        Button lean, remote, disarm, calibrate;
        bool leanHeld = false; // raw level, so a release cancels confirmation immediately
        bool seated = false;
        bool profileSettled = false;
        bool calibrationActive = false;
        int pendingProfile = NoProfile;
    };
    struct Actions
    {
        bool resetRemote = false;
        bool resetAssist = false;
        int activateProfile = NoProfile;
    };
    struct Transition
    {
        bool changed = false;
        bool seatDisarm = false;
        bool resetLean = false;
    };

    void begin(common::mode initial);
    void requestDisarm();
    Actions update(const Inputs &input, uint32_t now);
    // Called at the control tick after refreshing the seat sensors.
    Transition applySeat(bool seated, uint32_t now);
    bool takeCalibrationRequest();
    common::mode selected() const { return selectedMode_; }
    common::mode current() const { return currentMode_; }
    bool requiresSeat() const { return selectedMode_ == common::LEAN || selectedMode_ == common::ASSIST; }
    bool assistLEDOn(uint32_t now) const { return ((now - assistEnteredAt_) / 500) % 2 == 0; }

private:
    common::mode selectedMode_ = common::REMOTE;
    common::mode currentMode_ = common::REMOTE;
    bool leanPending_ = false, remotePending_ = false, disarmPending_ = false;
    bool assistHoldPending_ = false, calibrationPending_ = false;
    uint32_t disarmPressedAt_ = 0, profilePressedAt_ = 0, assistEnteredAt_ = 0;
    // When a drive mode was last left for DISARM (valid while leftDriveMode_).
    uint32_t disarmedAt_ = 0;
    bool leftDriveMode_ = false;
    int confirmingProfile_ = NoProfile;
};
