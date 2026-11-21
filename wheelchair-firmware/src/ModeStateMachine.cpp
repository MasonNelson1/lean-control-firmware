#include "ModeStateMachine.h"

using namespace common;

void ModeStateMachine::begin(mode initial)
{
    *this = ModeStateMachine{};
    selectedMode_ = currentMode_ = initial;
}

void ModeStateMachine::requestDisarm()
{
    selectedMode_ = DISARM;
    leanPending_ = remotePending_ = disarmPending_ = assistHoldPending_ = false;
    calibrationPending_ = false;
    confirmingProfile_ = NoProfile;
}

bool ModeStateMachine::needsSeatSample(const Inputs &in) const
{
    return in.lean.edge || in.remote.edge || in.disarm.edge ||
           leanPending_ || remotePending_ || disarmPending_ ||
           assistHoldPending_ || confirmingProfile_ != NoProfile;
}

ModeStateMachine::Actions ModeStateMachine::update(const Inputs &in, uint32_t now)
{
    Actions actions;
    if (in.calibrationActive)
    {
        // Poll buttons normally, but discard presses instead of queuing a mode change.
        leanPending_ = remotePending_ = disarmPending_ = assistHoldPending_ = false;
        calibrationPending_ = false;
        confirmingProfile_ = NoProfile;
        return actions;
    }
    leanPending_ |= in.lean.edge;
    remotePending_ |= in.remote.edge;
    disarmPending_ |= in.disarm.edge;
    calibrationPending_ |= in.calibrate.pressed && selectedMode_ == LEAN;

    if (confirmingProfile_ != in.pendingProfile || !in.profileSettled ||
        !in.leanHeld || currentMode_ != DISARM || selectedMode_ != DISARM)
        confirmingProfile_ = NoProfile;

    if ((leanPending_ || remotePending_ || disarmPending_ || assistHoldPending_ ||
         confirmingProfile_ != NoProfile) && !in.seated)
    {
        leanPending_ = remotePending_ = disarmPending_ = assistHoldPending_ = false;
        confirmingProfile_ = NoProfile;
        return actions;
    }

    const bool leanPressed = in.lean.pressed && leanPending_;
    const bool remotePressed = in.remote.pressed && remotePending_;
    const bool disarmPressed = in.disarm.pressed && disarmPending_;
    if (leanPressed) leanPending_ = false;
    if (remotePressed) remotePending_ = false;
    if (disarmPressed)
    {
        disarmPending_ = false;
        disarmPressedAt_ = now;
    }
    if (in.lean.released) leanPending_ = false;
    if (in.remote.released) remotePending_ = false;
    if (in.disarm.released)
        disarmPending_ = assistHoldPending_ = false;

    const bool profilePress = leanPressed && in.pendingProfile != NoProfile;
    if (profilePress && in.profileSettled && currentMode_ == DISARM && selectedMode_ == DISARM)
    {
        confirmingProfile_ = in.pendingProfile;
        profilePressedAt_ = now;
    }
    if (disarmPressed || remotePressed || assistHoldPending_)
        confirmingProfile_ = NoProfile;
    if (confirmingProfile_ != NoProfile && now - profilePressedAt_ >= 2000)
    {
        actions.activateProfile = confirmingProfile_;
        confirmingProfile_ = NoProfile;
    }

    if (disarmPressed)
    {
        // Exiting Assist requires a fresh press before it can be entered again.
        assistHoldPending_ = selectedMode_ != ASSIST;
        selectedMode_ = DISARM;
    }
    else if (leanPressed && !profilePress && selectedMode_ != LEAN)
    {
        assistHoldPending_ = false;
        selectedMode_ = LEAN;
    }
    else if (remotePressed && selectedMode_ != REMOTE)
    {
        assistHoldPending_ = false;
        selectedMode_ = REMOTE;
        actions.resetRemote = true;
    }
    else if (assistHoldPending_ && now - disarmPressedAt_ >= 500)
    {
        assistHoldPending_ = false;
        actions.resetAssist = true;
        selectedMode_ = ASSIST;
    }
    return actions;
}

ModeStateMachine::Transition ModeStateMachine::applySeat(bool seated, uint32_t now)
{
    const bool seatDisarm = requiresSeat() && !seated;
    const mode next = seatDisarm ? DISARM : selectedMode_;
    if (next == currentMode_) return {};
    currentMode_ = next;
    if (next == ASSIST) assistEnteredAt_ = now;
    return {true, seatDisarm, next == LEAN};
}

bool ModeStateMachine::takeCalibrationRequest()
{
    const bool requested = calibrationPending_;
    calibrationPending_ = false;
    return requested;
}
