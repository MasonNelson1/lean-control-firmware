#pragma once

#include "Protocol.h"
#include "../common.h"

namespace settings
{

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
    // Called before each EEPROM byte is written. main.cpp points it at the
    // watchdog so a long save cannot reset the Teensy halfway through.
    extern void (*storageActivity)();

    extern config::EnumField<common::mode, 4> defaultMode;
    extern config::ConfigField<float> deadband;
    // Bounds shared with the calibration code that writes these fields.
    constexpr int LeanRangeMin = 40, LeanRangeMax = 3000;
    constexpr int SensorZeroMin = 100, SensorZeroMax = 1500;
    // Allowed exponents of the two lean response curves: 1 is a straight line.
    constexpr float LeanCurveExponentMin = 1.0f, LeanCurveExponentMax = 4.0f;
    extern config::ConfigField<int> rangeForward, rangeBack, rangeLeft, rangeRight;
    extern config::ConfigField<int> zeroFront, zeroBack, zeroLeft, zeroRight, seatThreshold;
    extern config::ConfigField<float> maxTurnRate, maxLateralAccel, trackWidth, vescMinSpeed;
    extern config::ProfileField<2> leanForewardBackResponse;
    extern config::ProfileField<2> leanSideToSideResponse;
    extern config::ProfileField<2> assistResponse;
    extern config::ConfigField<float> assistMaxBoostAmps, assistSlowdownRPM, assistSlowdownBoostFraction;

    extern config::ConfigField<unsigned long> controllerTimeout, buttonDebounce;
    extern config::ConfigField<uint32_t> controlPeriod;
    extern config::ConfigField<uint8_t> leftVesc, rightVesc;
    extern config::ConfigField<float> wheelCircumference;

    // Each profile owns six consecutive stable field IDs beginning at firstId.
    // tuning() builds a controller tuning object while retaining hard acceleration caps.
    struct PidProfile
    {
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
