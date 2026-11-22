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
        Group gains(registry(), 3, "Plus layout gains", false, &lean);
        // Group ID 4 is retired with the directional saturation factors.
        Group response(registry(), 5, "Response curves", false, &lean);
        Group timing(registry(), 6, "Communication and timing", true);
        Group calibration(registry(), 7, "Calibration", true);
        Group motor(registry(), 8, "Motor configuration", true);
        Group pid(registry(), 9, "Wheel RPM PID", true);
        Group normal(registry(), 10, "Normal operation", true, &pid);
        Group transition(registry(), 11, "Transitions (reserved)", true, &pid);
        Group corner(registry(), 12, "Corner layout reach", false, &lean);
        Group cornerSensors(registry(), 13, "Corner layout sensors", true);

        constexpr Choice Modes[] = {{common::LEAN, "LEAN"}, {common::REMOTE, "REMOTE"}, {common::DISARM, "DISARM"}, {common::ASSIST, "ASSIST"}};
        constexpr Choice Layouts[] = {{static_cast<uint8_t>(lean::SensorLayout::Plus), "PLUS"},
                                      {static_cast<uint8_t>(lean::SensorLayout::Corner), "CORNER"}};
        constexpr float DefaultForewardBackKnee[] = {0.21f, 0.09f};
        constexpr float DefaultAssistKnee[] = {0.44f, 0.61f};
        constexpr float DefaultLeftRightKnee[] = {0.35f, 0.20f};
        // Fit two segments to (speed + 0.1)^0.47 over positive speed.
        constexpr float FullSpeedTurnGain = 1.0458f;
        constexpr float DefaultTurnMix[] = {0.0f, 0.3388f, 0.293f, 0.6447f, 1.0f, FullSpeedTurnGain};
        constexpr Parameter TurnMixBounds[] = {
            {"Zero speed", 0.0f, 0.0f}, {"Turn gain at zero speed", 0.0f, FullSpeedTurnGain},
            {"Knee speed", 0.001f, 0.999f}, {"Knee gain", 0.0f, FullSpeedTurnGain},
            {"Full speed", 1.0f, 1.0f}, {"Full-speed gain", FullSpeedTurnGain, FullSpeedTurnGain}
        };
        constexpr Parameter KneeBounds[] = {{"Knee input", 0.001f, 0.999f}, {"Knee output", 0.001f, 0.999f}};
        constexpr auto DefaultPid = RPMSetpointPID::defaultTuning();

        // EEPROM.update avoids programming an unchanged byte. Persistence controls
        // bank selection, integrity checking, and the final commit marker.
        class EepromStorage : public Storage
        {
            uint8_t read(size_t address) const override { return EEPROM.read(address); }
            void update(size_t address, uint8_t value) override { EEPROM.update(address, value); }
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
                                {drive, "Lean deadband", "Normalized lean input below this magnitude is treated as zero before shaping. Plus layout only; the corner layout has its own deadzone in inches."},
                                0.05f, 0.01f, 1.0f);

    ConfigField<float> frontGain(registry(), 10,
                                 {gains, "Front gain", "Multiplier applied to the front sensor after subtracting its calibrated floor. Plus layout only.", "×"}, 2, 0.01f, 100);
    ConfigField<float> rearGain(registry(), 11,
                                {gains, "Rear gain", "Multiplier applied to the rear sensor after subtracting its calibrated floor. Plus layout only.", "×"}, 3, 0.01f, 100);
    ConfigField<float> leftGain(registry(), 12,
                                {gains, "Left gain", "Multiplier applied to the left sensor after subtracting its calibrated floor. Plus layout only.", "×"}, 1, 0.01f, 100);
    ConfigField<float> rightGain(registry(), 13,
                                 {gains, "Right gain", "Multiplier applied to the right sensor after subtracting its calibrated floor. Plus layout only.", "×"}, 1, 0.01f, 100);

    // Field IDs 20-23 are retired; normalize lean differences by rider weight.

    // Sensor layout and the corner layout's settings (IDs 90-107). The math is in
    // driveModes/lean_layout.h, which also holds the defaults and the sensor
    // positions. These 18 fields take 109 bytes of each saved profile, leaving
    // about 7 (the "every setting fits one saved profile" test guards this).
    EnumField<lean::SensorLayout, 2> sensorLayout(registry(), 90,
                                                  {lean, "Sensor layout", "PLUS: one sensor in front of, behind, left and right of centre, steering from front-minus-back and right-minus-left. CORNER: one sensor at each corner, steering from the rider's centre of pressure. Takes effect at the next calibration."},
                                                  lean::SensorLayout::Corner, Layouts);
    ConfigField<float> cornerDeadzone(registry(), 91,
                                      {corner, "Deadzone", "Centre-of-pressure travel from neutral, per axis, that is ignored. Output ramps up from zero at its edge.", "in"},
                                      lean::DefaultDeadzone, 0.0f, lean::MaxDeadzone);
    ConfigField<float> reachForward(registry(), 92,
                                    {corner, "Full lean forward", "Centre-of-pressure travel forward of neutral (of the relaxed posture, when one is captured) that gives full forward speed. A dynamic calibration overrides this for the current rider.", "in"},
                                    lean::DefaultReachForward, lean::MinReach, lean::MaxReach);
    ConfigField<float> reachBack(registry(), 93,
                                 {corner, "Full lean back", "Distance behind neutral (a positive number) that gives full reverse. A dynamic calibration overrides this for the current rider.", "in"},
                                 lean::DefaultReachBack, lean::MinReach, lean::MaxReach);
    ConfigField<float> reachLeft(registry(), 94,
                                 {corner, "Full lean left", "Distance left of neutral (a positive number) that gives a full left turn. A dynamic calibration overrides this for the current rider.", "in"},
                                 lean::DefaultReachLeft, lean::MinReach, lean::MaxReach);
    ConfigField<float> reachRight(registry(), 95,
                                  {corner, "Full lean right", "Distance right of neutral that gives a full right turn. A dynamic calibration overrides this for the current rider.", "in"},
                                  lean::DefaultReachRight, lean::MinReach, lean::MaxReach);
    ConfigField<float> passiveMargin(registry(), 96,
                                     {corner, "Relaxed posture margin", "After a second calibrate press captures the relaxed posture, speed stays zero from the upright to the relaxed posture plus this margin at each end. Replaces the deadzone on the forward/back axis.", "in"},
                                     lean::DefaultPassiveMargin, 0.0f, lean::MaxDeadzone);

    // Channel names are the ADC pins'. On today's chair F = back-left, B = back-right,
    // L = front-left, R = front-right (lean::CornerPositions).
    ConfigField<uint16_t> emptyPlateF(registry(), 100,
                                      {cornerSensors, "Empty plate F (back-left)", "Raw reading of channel F with the plate on and nobody seated. Force is counted above this.", "ADC counts"},
                                      lean::DefaultEmptyPlateF, 0, 4095);
    ConfigField<uint16_t> emptyPlateB(registry(), 101,
                                      {cornerSensors, "Empty plate B (back-right)", "Raw reading of channel B with the plate on and nobody seated. Force is counted above this.", "ADC counts"},
                                      lean::DefaultEmptyPlateB, 0, 4095);
    ConfigField<uint16_t> emptyPlateL(registry(), 102,
                                      {cornerSensors, "Empty plate L (front-left)", "Raw reading of channel L with the plate on and nobody seated. Force is counted above this.", "ADC counts"},
                                      lean::DefaultEmptyPlateL, 0, 4095);
    ConfigField<uint16_t> emptyPlateR(registry(), 103,
                                      {cornerSensors, "Empty plate R (front-right)", "Raw reading of channel R with the plate on and nobody seated. Force is counted above this.", "ADC counts"},
                                      lean::DefaultEmptyPlateR, 0, 4095);
    // Gains default to 1. E10's bare-sensor test hinted back-right read about half,
    // but the plate-on known-weight check (E11) located 28.2 lb within 0.4" with all
    // gains at 1, so the system needs no correction today.
    ConfigField<float> cornerGainF(registry(), 104,
                                   {cornerSensors, "Gain F (back-left)", "Multiplier on channel F's force above the empty plate, to even out sensor sensitivity.", "×"}, 1.0f, 0.1f, 10.0f);
    ConfigField<float> cornerGainB(registry(), 105,
                                   {cornerSensors, "Gain B (back-right)", "Multiplier on channel B's force above the empty plate, to even out sensor sensitivity.", "×"}, 1.0f, 0.1f, 10.0f);
    ConfigField<float> cornerGainL(registry(), 106,
                                   {cornerSensors, "Gain L (front-left)", "Multiplier on channel L's force above the empty plate, to even out sensor sensitivity.", "×"}, 1.0f, 0.1f, 10.0f);
    ConfigField<float> cornerGainR(registry(), 107,
                                   {cornerSensors, "Gain R (front-right)", "Multiplier on channel R's force above the empty plate, to even out sensor sensitivity.", "×"}, 1.0f, 0.1f, 10.0f);


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

    ProfileField<6> leanTurnMix(registry(), 36,
                               {response, "Lean turn mixing", "Turn multiplier versus absolute shaped speed. The point on the vertical axis sets the turn gain at zero speed."},
                               Editor::PiecewiseLinear, DefaultTurnMix, TurnMixBounds);

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

    // Left = 2, right = 26: the real IDs on the chair (confirmed by Mason 2026-11-21). Don't swap.
    ConfigField<uint8_t> leftVesc(registry(), 60,
                                  {motor, "Left VESC ID", "CAN address of the left motor controller. The two addresses must differ."}, 2, 0, 253);
    ConfigField<uint8_t> rightVesc(registry(), 61,
                                   {motor, "Right VESC ID", "CAN address of the right motor controller. The two addresses must differ."}, 26, 0, 253);
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
