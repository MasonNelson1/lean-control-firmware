#include "DriveSettings.h"

#include <EEPROM.h>

namespace settings
{
    using namespace config;

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
        Group gains(registry(), 3, "Sensor gains", false, &lean);
        Group sensitivity(registry(), 4, "Sensor sensitivity", false, &lean);
        Group response(registry(), 5, "Response curves", false, &lean);
        Group timing(registry(), 6, "Communication and timing", true);
        Group calibration(registry(), 7, "Calibration", true);
        Group motor(registry(), 8, "Motor configuration", true);
        Group pid(registry(), 9, "Wheel RPM PID", true);
        Group normal(registry(), 10, "Normal operation", true, &pid);
        Group transition(registry(), 11, "Transitions (reserved)", true, &pid);

        constexpr Choice Modes[] = {{common::LEAN, "LEAN"}, {common::REMOTE, "REMOTE"}};
        constexpr float DefaultKnee[] = {0.5f, 0.25f};
        constexpr Parameter KneeBounds[] = {{"Knee input", 0.001f, 0.999f}, {"Knee output", 0.001f, 0.999f}};
        constexpr auto DefaultPid = RPMSetpointPID::defaultTuning();

        // EEPROM.update avoids programming an unchanged byte. Persistence controls
        // bank selection, integrity checking, and the final commit marker.
        class EepromStorage : public Storage
        {
            uint8_t read(size_t address) const override { return EEPROM.read(address); }
            void update(size_t address, uint8_t value) override { EEPROM.update(address, value); }
        } eeprom;
        static_assert(E2END + 1 >= 2 * Persistence::SlotSize, "Configuration slots exceed EEPROM capacity");
    } // namespace

    // IDs are part of the storage format. Keep them stable when moving or renaming a
    // field, and never reuse a retired ID. Add a declaration and consume its get()
    // value to expose a setting through discovery. Cached consumers also need their
    // derived values refreshed in main.cpp's applyRuntimeSettings().
    EnumField<common::mode, 2> defaultMode(registry(), 1,
                                           {drive, "Startup drive mode", "Drive mode selected at power-on. LEAN requires calibration and REMOTE requires a fresh controller command.", "", true},
                                           common::REMOTE, Modes);
    ConfigField<float> deadband(registry(), 2,
                                {drive, "Lean deadband", "Normalized lean input below this magnitude is treated as zero before shaping."},
                                0.05f, 0.01f, 1.0f);

    ConfigField<float> frontGain(registry(), 10,
                                 {gains, "Front gain", "Multiplier applied to the front sensor after subtracting its calibrated floor.", "×"}, 1, 0.01f, 100);
    ConfigField<float> rearGain(registry(), 11,
                                {gains, "Rear gain", "Multiplier applied to the rear sensor after subtracting its calibrated floor.", "×"}, 4, 0.01f, 100);
    ConfigField<float> leftGain(registry(), 12,
                                {gains, "Left gain", "Multiplier applied to the left sensor after subtracting its calibrated floor.", "×"}, 1, 0.01f, 100);
    ConfigField<float> rightGain(registry(), 13,
                                 {gains, "Right gain", "Multiplier applied to the right sensor after subtracting its calibrated floor.", "×"}, 1, 0.01f, 100);

    ConfigField<float> frontWeight(registry(), 20,
                                   {sensitivity, "Front saturation factor", "Multiple of the calibrated weight reference for full forward input. Higher values require more force. Recalibrate to apply.", "×"}, 1, .2, 3);
    ConfigField<float> rearWeight(registry(), 21,
                                  {sensitivity, "Rear saturation factor", "Multiple of the calibrated weight reference for full reverse input. Higher values require more force. Recalibrate to apply.", "×"}, 1, .2, 3);
    ConfigField<float> leftWeight(registry(), 22,
                                  {sensitivity, "Left saturation factor", "Multiple of the calibrated weight reference for full left turn. Higher values require more force. Recalibrate to apply.", "×"}, 1, .2, 3);
    ConfigField<float> rightWeight(registry(), 23,
                                   {sensitivity, "Right saturation factor", "Multiple of the calibrated weight reference for full right turn. Higher values require more force. Recalibrate to apply.", "×"}, 1, .2, 3);

    // The knee is one atomic value, so input/output constraints can be checked
    // together. LeanDriveMode derives its exponent from the accepted coordinates.
    ProfileField<2> leanResponse(registry(), 30,
                                 {response, "Lean response", "Signed power curve for normalized speed and turn after the deadband. Lower knee output softens small inputs."},
                                 Editor::SignedPower, DefaultKnee, KneeBounds);

    // These bounds keep the watchdog at or below its original timeout and the
    // control loop at 100 Hz or faster. Hard propulsion limits stay in drive_constants.h.
    ConfigField<unsigned long> controllerTimeout(registry(), 40,
                                                 {timing, "Controller timeout", "Maximum time since the last accepted controller packet before remote targets go to zero.", "ms"}, 100, 30, 500);
    ConfigField<unsigned long> buttonDebounce(registry(), 41,
                                              {timing, "Button debounce", "Time a physical button state must remain stable before the firmware accepts it.", "ms"}, 50, 5, 150);
    ConfigField<uint32_t> controlPeriod(registry(), 42,
                                        {timing, "Control period", "Interval between wheel command / PID updates. ", "ms"}, 5, 2, 15);

    ConfigField<int> calibrationSamples(registry(), 50,
                                        {calibration, "Calibration samples", "Number of readings averaged per sensor when the calibration button is pressed.", "samples"}, 20, 5, 500);
    ConfigField<int> defaultFloor(registry(), 51,
                                  {calibration, "Default sensor floor", "ADC baseline used for sensor telemetry until a successful calibration. Calibration is still required to drive in LEAN.", "ADC counts"}, 400, 0, 1024);

    ConfigField<uint8_t> leftVesc(registry(), 60,
                                  {motor, "Left VESC ID", "CAN address of the left motor controller. The two addresses must differ."}, 26, 0, 253);
    ConfigField<uint8_t> rightVesc(registry(), 61,
                                   {motor, "Right VESC ID", "CAN address of the right motor controller. The two addresses must differ."}, 2, 0, 253);
    ConfigField<float> wheelCircumference(registry(), 62,
                                          {motor, "Wheel circumference", "Measured rolling circumference. Updates LINEAR_TO_RPM for wheel speed and acceleration conversion.", "m"},
                                          common::WHEEL_CIRCUMFERENCE_M, 1.80f, 2.00f);

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
