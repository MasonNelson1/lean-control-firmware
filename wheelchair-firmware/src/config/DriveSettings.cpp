#include "DriveSettings.h"

#include <EEPROM.h>

namespace settings
{
    using namespace config;

    void (*storageActivity)() = nullptr;

    Registry &registry()
    {
        // A function-local instance is ready when any translation unit registers a
        // global field, regardless of global initialization order between files.
        static Registry fields;
        return fields;
    }

    namespace
    {
        // Construct roots before child groups, and all groups before their fields.
        Group drive(registry(), 1, "Drive behavior");
        Group lean(registry(), 2, "Lean control");
        // Group ID 3 (per-sensor gains) is retired with those gains.
        // Group ID 4 is retired with the directional saturation factors.
        Group range(registry(), 12, "Full-lean range", false, &lean);
        Group response(registry(), 5, "Response curves", false, &lean);
        Group timing(registry(), 6, "Communication and timing", true);
        Group calibration(registry(), 7, "Calibration", true);
        Group motor(registry(), 8, "Motor configuration", true);
        Group pid(registry(), 9, "Wheel RPM PID", true);
        Group normal(registry(), 10, "Normal operation", true, &pid);
        Group transition(registry(), 11, "Transitions (reserved)", true, &pid);

        constexpr Choice Modes[] = {{common::LEAN, "LEAN"}, {common::REMOTE, "REMOTE"}, {common::DISARM, "DISARM"}, {common::ASSIST, "ASSIST"}};
        constexpr float DefaultForewardBackKnee[] = {0.21f, 0.09f};
        constexpr float DefaultAssistKnee[] = {0.44f, 0.61f};
        constexpr float DefaultLeftRightKnee[] = {0.35f, 0.20f};
        constexpr Parameter KneeBounds[] = {{"Knee input", 0.001f, 0.999f}, {"Knee output", 0.001f, 0.999f}};
        constexpr auto DefaultPid = RPMSetpointPID::defaultTuning();

        // EEPROM.update avoids programming an unchanged byte. Persistence controls
        // bank selection, integrity checking, and the final commit marker.
        class EepromStorage : public Storage
        {
            uint8_t read(size_t address) const override { return EEPROM.read(address); }
            void update(size_t address, uint8_t value) override
            {
                // A save can take longer than the watchdog allows (flash erases).
                if (storageActivity)
                    storageActivity();
                EEPROM.update(address, value);
            }
        } eeprom;
        static_assert(E2END + 1 >= Persistence::StorageSize, "Configuration slots exceed EEPROM capacity");
    } // namespace

    // IDs are part of the storage format. Keep them stable when moving or renaming a
    // field, and never reuse a retired ID. Add a declaration and consume its get()
    // value to expose a setting through discovery. Cached consumers also need their
    // derived values refreshed in main.cpp's applyRuntimeSettings().
    EnumField<common::mode, 4> defaultMode(registry(), 1,
                                           {drive, "Startup drive mode", "Drive mode selected at power-on. LEAN requires calibration and REMOTE requires a fresh controller command.", "", true},
                                           common::DISARM, Modes);
    ConfigField<float> deadband(registry(), 2,
                                {drive, "Lean deadband", "Share of a full lean that is ignored around neutral. The response starts from zero at its edge."},
                                0.05f, 0.01f, 0.5f);

    // Field IDs 10-13 (per-sensor gains) are retired. A gain on one sensor
    // turned any change in total load (a bump, shifting in the seat) into
    // motion whenever the two gains of a pair differed. The four ranges below
    // scale the front-minus-back and right-minus-left differences instead.
    // Field IDs 20-23 are retired; normalize lean differences by rider weight.

    // Full-lean ranges, in ADC counts of pair difference beyond neutral. Hold
    // the calibration button for one second to measure them for the rider.
    // Defaults are from the 2026-11 driving log (run2): that rider reached
    // over 730 counts forward and sideways and about 420 back. A default that
    // is too large only makes the chair less sensitive until it is calibrated.
    ConfigField<int> rangeForward(registry(), 14,
                                  {range, "Forward", "Front-minus-back difference, beyond neutral, that commands full forward speed. Set by the range calibration (hold the calibration button for one second).", "counts", false, true},
                                  600, LeanRangeMin, LeanRangeMax);
    ConfigField<int> rangeBack(registry(), 15,
                               {range, "Back", "Back-minus-front difference, beyond neutral, that commands full reverse speed. Set by the range calibration.", "counts", false, true},
                               400, LeanRangeMin, LeanRangeMax);
    ConfigField<int> rangeLeft(registry(), 16,
                               {range, "Left", "Left-minus-right difference, beyond neutral, that commands a full left turn. Set by the range calibration.", "counts", false, true},
                               500, LeanRangeMin, LeanRangeMax);
    ConfigField<int> rangeRight(registry(), 17,
                                {range, "Right", "Right-minus-left difference, beyond neutral, that commands a full right turn. Set by the range calibration.", "counts", false, true},
                                500, LeanRangeMin, LeanRangeMax);

    // The knee is one atomic value, so input/output constraints can be checked
    // together. LeanDriveMode derives its exponent from the accepted coordinates.
    ProfileField<2> leanForewardBackResponse(registry(), 30,
                                             {response, "Lean foreward response", "Signed power curve for normalized speed after the deadband. Lower knee output softens small inputs."},
                                             Editor::SignedPower, DefaultForewardBackKnee, KneeBounds);
    ProfileField<2> leanSideToSideResponse(registry(), 31,
                                           {response, "Lean turning response", "Signed power curve for normalized turn after the deadband. Lower knee output softens small inputs."},
                                           Editor::SignedPower, DefaultLeftRightKnee, KneeBounds);
    ProfileField<2> assistResponse(registry(), 32,
                                   {response, "Assist response", "Signed power curve for normalized assist power "},
                                   Editor::SignedPower, DefaultAssistKnee, KneeBounds);
    ConfigField<float> assistMaxBoostAmps(registry(), 33,
                                         {drive, "Assist max boost", "Maximum requested boost current per motor.", "A"},
                                         25.0f, 5.0f, 30.0f);
    ConfigField<float> assistSlowdownRPM(registry(), 34,
                                        {drive, "Assist slowdown threshold", "Drop below the filtered wheel speed that triggers reduced boost on that wheel.", "RPM"},
                                        4.0f, 1.0f, 5.0f);
    ConfigField<float> assistSlowdownBoostFraction(registry(), 35,
                                                  {drive, "Assist slowdown boost fraction", "Fraction of normal boost delivered to a slowing wheel. 0.3 delivers 30% of normal boost."},
                                                  0.3f, 0.1f, 0.5f);

    // Field ID 36 (turn gain versus speed, as a share of the speed knob) is
    // retired. Turning is now a rate in deg/s, the same on every speed-knob
    // position, limited at speed by sideways acceleration.
    //
    // 160 deg/s is what full lean gave at a standstill on speed-knob position 2
    // with the old turn mix, which is where the team has been driving. CDR
    // Spec H asks for 200 deg/s; raise it here once turning feels controlled.
    ConfigField<float> maxTurnRate(registry(), 37,
                                   {response, "Full-lean turn rate", "How fast the chair rotates at full sideways lean. The same on every speed-knob position.", "deg/s"},
                                   160.0f, 20.0f, 360.0f);
    // Turn rate at speed is capped so that speed x turn rate stays under this.
    // 4 m/s^2 is about 0.4 g: at 2.5 m/s the tightest turn is a 1.5 m radius.
    ConfigField<float> maxLateralAccel(registry(), 38,
                                       {response, "Sideways acceleration limit", "Limits turn rate at speed so the chair cannot be thrown into a turn it would tip or slide in. Lower is gentler.", "m/s²"},
                                       4.0f, 1.0f, 15.0f);

    // CDR Spec B wants a safe state within 0.1 s of a fault. The timeout default
    // meets it; the upper bounds stop a setting from drifting far past it.
    ConfigField<unsigned long> controllerTimeout(registry(), 40,
                                                 {timing, "Controller timeout", "Maximum time since the last accepted controller packet before the chair brakes to a stop.", "ms"}, 100, 30, 200);
    ConfigField<unsigned long> buttonDebounce(registry(), 41,
                                              {timing, "Button debounce", "Time a physical button state must remain stable before the firmware accepts it.", "ms"}, 50, 5, 100);
    ConfigField<uint32_t> controlPeriod(registry(), 42,
                                        {timing, "Control period", "Interval between wheel command / PID updates. ", "ms"}, 5, 2, 15);

    // Field IDs 50 (calibration sample count) and 51 (one shared sensor floor)
    // are retired. Neutral is now averaged over a fixed still second, and each
    // sensor has its own zero.
    //
    // An unloaded sensor reads about 372 counts (amplifier offset, measured on
    // the chair 2026-11-21), but the back sensor rests near 437 because the
    // seat is angled back. Tare with the seat empty to measure all four: in
    // DISARM, hold the calibration button for two seconds.
    ConfigField<int> zeroFront(registry(), 52,
                               {calibration, "Front sensor zero", "Reading of the front sensor with the seat empty. Set by the tare (in DISARM with the seat empty, hold the calibration button for two seconds).", "ADC counts", false, true},
                               372, SensorZeroMin, SensorZeroMax);
    ConfigField<int> zeroBack(registry(), 53,
                              {calibration, "Back sensor zero", "Reading of the back sensor with the seat empty. Set by the tare.", "ADC counts", false, true},
                              372, SensorZeroMin, SensorZeroMax);
    ConfigField<int> zeroLeft(registry(), 54,
                              {calibration, "Left sensor zero", "Reading of the left sensor with the seat empty. Set by the tare.", "ADC counts", false, true},
                              372, SensorZeroMin, SensorZeroMax);
    ConfigField<int> zeroRight(registry(), 55,
                               {calibration, "Right sensor zero", "Reading of the right sensor with the seat empty. Set by the tare.", "ADC counts", false, true},
                               372, SensorZeroMin, SensorZeroMax);
    // Total load (all four sensors, above their zeros) that counts as a rider.
    // 200 is the CDR's seat threshold. One logged rider sat at about 330 with
    // the seat neutral. The seat counts as empty again below 60% of this.
    ConfigField<int> seatThreshold(registry(), 56,
                                   {calibration, "Seat load threshold", "Total sensor load above which a rider is seated. The dashboard's total force shows the live value.", "counts", false, true},
                                   200, 40, 2000);

    // Left = 2, right = 26: the real IDs on the chair (confirmed by Mason 2026-11-21). Don't swap.
    ConfigField<uint8_t> leftVesc(registry(), 60,
                                  {motor, "Left VESC ID", "CAN address of the left motor controller. The two addresses must differ."}, 2, 0, 253);
    ConfigField<uint8_t> rightVesc(registry(), 61,
                                   {motor, "Right VESC ID", "CAN address of the right motor controller. The two addresses must differ."}, 26, 0, 253);
    ConfigField<float> wheelCircumference(registry(), 62,
                                          {motor, "Wheel circumference", "Measured rolling circumference. Updates LINEAR_TO_RPM for wheel speed and acceleration conversion.", "m"},
                                          common::WHEEL_CIRCUMFERENCE_M, 1.80f, 2.00f);
    // Distance between the two drive wheels where they touch the floor. It
    // converts a turn rate into a wheel-speed difference. 0.60 m is an
    // estimate: measure it on the chair (cambered wheels are wider at the floor).
    ConfigField<float> trackWidth(registry(), 63,
                                  {motor, "Track width", "Distance between the drive wheels' contact patches. Converts turn rate (deg/s) to wheel speeds: too small a value makes the chair turn slower than the setting.", "m", false, true},
                                  0.60f, 0.30f, 1.20f);

    PidProfile::PidProfile(Group &group, uint16_t firstId)
        : lowTau(registry(), firstId,
                 {group, "Low-speed time constant", "Response time near zero wheel speed. A larger value makes the command respond more gradually.", "s"}, DefaultPid.lowSpeedTauS, 0.005f, 0.5f),
          highTau(registry(), firstId + 1,
                  {group, "High-speed time constant", "Response time near the wheel speed limit. The smoother blends between the two time constants.", "s"}, DefaultPid.highSpeedTauS, 0.03f, 1.50f),
          ki(registry(), firstId + 2,
             {group, "Integral gain", "Weight of accumulated RPM error. Zero disables the integral term.", "1/s²"}, DefaultPid.ki, 0, 5),
          kd(registry(), firstId + 3,
             {group, "Derivative gain", "Damping applied to the rate of change of smoothed wheel RPM."}, DefaultPid.kd, 0, 3),
          filter(registry(), firstId + 4,
                 {group, "Derivative filter", "Time constant of the derivative low-pass filter.", "s"}, DefaultPid.derivativeFilterS, 0.0f, 3),
          maxJerk(registry(), firstId + 5,
                  {group, "Maximum jerk", "Maximum rate of change of wheel acceleration during ordinary smoothing.", "RPM/s²"}, DefaultPid.maxJerkRPMPerS2, 10, 3000),
          previewFields{lowTau.id, highTau.id, ki.id, kd.id, filter.id, maxJerk.id, controlPeriod.id, wheelCircumference.id},
          previewLimits{common::MAX_SPEED, common::MAX_ACCEL},
          preview{1, previewFields, 8, previewLimits, 2}
    {
        group.preview = &preview;
    }

    RPMSetpointPID::Tuning PidProfile::tuning() const
    {
        // Only tuning parameters are editable. Linear acceleration limits remain
        // derived from the firmware's fixed design cap for both speed regions.
        return {lowTau.get(), highTau.get(), ki.get(), kd.get(), filter.get(),
                common::MAX_ACCEL, common::MAX_ACCEL, maxJerk.get()};
    }

    PidProfile normalPid(normal, 70);
    PidProfile transitionPid(transition, 80);

    static bool validDriveSettings() { return leftVesc.get() != rightVesc.get(); }

    static void repairDriveSettings()
    {
        // Each saved CAN ID may be individually valid but conflict with the other.
        // Repair this pair after load so unrelated migrated settings are preserved.
        if (!validDriveSettings())
        {
            leftVesc.reset();
            rightVesc.reset();
        }
    }

    // Persistence repairs migrated values on load. Protocol uses the same invariant
    // to reject conflicting live edits and rolls them back before acknowledging.
    Persistence persistence(registry(), eeprom, repairDriveSettings);
    Protocol protocol(registry(), persistence, validDriveSettings);

} // namespace settings
