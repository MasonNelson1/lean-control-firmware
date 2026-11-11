#pragma once

#include "Protocol.h"
#include "../common.h"

namespace settings {

/**
 * Wheelchair-specific declarations built on the generic config framework.
 * Read current RAM values with get(). Add fields here for shared access, or
 * declare them in their owning subsystem and register with registry().
 *
 * IDs, defaults, groups, descriptions, and bounds live in DriveSettings.cpp.
 * See docs/CONFIGURATION.md for examples of extending the schema.
 */
config::Registry &registry();
extern config::Persistence persistence;
extern config::Protocol protocol;

extern config::EnumField<common::mode, 2> defaultMode;
extern config::ConfigField<float> deadband;
extern config::ConfigField<float> frontGain, rearGain, leftGain, rightGain;
extern config::ConfigField<float> frontWeight, rearWeight, leftWeight, rightWeight;
extern config::ProfileField<2> leanResponse;

extern config::ConfigField<unsigned long> controllerTimeout, buttonDebounce;
extern config::ConfigField<uint32_t> controlPeriod;
extern config::ConfigField<int> calibrationSamples, defaultFloor;
extern config::ConfigField<uint8_t> leftVesc, rightVesc;
extern config::ConfigField<float> wheelCircumference;

// Each profile owns six consecutive stable field IDs beginning at firstId.
// tuning() builds a controller tuning object while retaining hard acceleration caps.
struct PidProfile {
    PidProfile(config::Group &group, uint16_t firstId);
    RPMSetpointPID::Tuning tuning() const;

    config::ConfigField<float> lowTau, highTau, ki, kd, filter, maxJerk;

private:
    // PID preview v1 roles are low/high tau, Ki, Kd, derivative filter, jerk,
    // control period, and circumference. Constants are maximum speed/acceleration.
    uint16_t previewFields[8];
    float previewLimits[2];
    config::GroupPreview preview;
};

extern PidProfile normalPid, transitionPid; // Transition tuning is reserved for future integration.

} // namespace settings
