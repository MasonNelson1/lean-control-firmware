#include "DriveSettings.h"

#include <EEPROM.h>
#include <math.h>

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

        constexpr Choice Layouts[] = {{static_cast<uint8_t>(lean::SensorLayout::Plus), "PLUS"},
                                      {static_cast<uint8_t>(lean::SensorLayout::Corner), "CORNER"}};
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
    // Field ID 1 (startup drive mode) is retired: the chair always powers on
    // in DISARM. Starting in a powered mode meant a reset or a flick of the
    // switch could put a rider in LEAN, REMOTE or Assist with no button pressed.
    ConfigField<float> deadband(registry(), 2,
                                {drive, "Lean deadband", "Normalized lean input below this magnitude is treated as zero before shaping. Plus layout only; the corner layout has its own deadzone in inches."},
                                0.05f, 0.01f, 0.5f); // council/firmware-9 bound (was 1.0)

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
                                             {response, "Lean foreward response", "Signed power curve for normalized speed after the deadband. Lower knee output softens small inputs. The knee must stay on or below the straight line (a curve that is steepest at neutral is refused)."},
                                             Editor::SignedPower, DefaultForewardBackKnee, KneeBounds);
    ProfileField<2> leanSideToSideResponse(registry(), 31,
                                           {response, "Lean turning response", "Signed power curve for normalized turn after the deadband. Lower knee output softens small inputs. The knee must stay on or below the straight line (a curve that is steepest at neutral is refused)."},
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
    // A chair with a 0.6 m track and the rider's centre of mass about 0.55 m
    // up starts to tip near 9.81 * 0.3 / 0.55 = 5.3 m/s^2. The default is
    // under half of that: at 2.5 m/s the tightest turn is a 2.5 m radius. The
    // upper bound stays just above the tipping estimate (camber and anti-tip
    // wheels give a real sports chair more), so it cannot be set far past it.
    ConfigField<float> maxLateralAccel(registry(), 38,
                                       {response, "Sideways acceleration limit", "Limits turn rate at speed so the chair cannot be thrown into a turn it would tip or slide in. Lower is gentler.", "m/s²"},
                                       2.5f, 1.0f, 6.0f);

    // CDR Spec B wants a safe state within 0.1 s of a fault. The timeout default
    // meets it; the upper bounds stop a setting from drifting far past it.
    ConfigField<unsigned long> controllerTimeout(registry(), 40,
                                                 {timing, "Controller timeout", "Maximum time since the last accepted controller packet before the chair brakes to a stop.", "ms"}, 100, 30, 200);
    ConfigField<unsigned long> buttonDebounce(registry(), 41,
                                              {timing, "Button debounce", "Time a physical button state must remain stable before the firmware accepts it.", "ms"}, 50, 5, 100);
    ConfigField<uint32_t> controlPeriod(registry(), 42,
                                        {timing, "Control period", "Interval between wheel command / PID updates. ", "ms"}, 5, 2, 15);

    // Council/firmware-9 retired 50 and 51 with its own lean math. This build
    // keeps today's corner lean input, which still averages calibrationSamples
    // reads for neutral and uses defaultFloor for seat detection, so both stay.
    // (Council IDs 14-17 lean ranges and 52-56 sensor zeros / seat threshold are
    // not registered here: that lean math is not in this build.)
    ConfigField<int> calibrationSamples(registry(), 50,
                                        {calibration, "Calibration samples", "Number of readings averaged per sensor when the calibration button is pressed.", "samples"}, 20, 5, 500);
    ConfigField<int> defaultFloor(registry(), 51,
                                  {calibration, "Default sensor floor", "ADC baseline used for sensor telemetry until a successful calibration. Calibration is still required to drive in LEAN.", "ADC counts"}, 400, 0, 1024);

    // Left = 2, right = 26: the real IDs on the chair (confirmed by Mason 2026-11-21). Don't swap.
    // The motor fields belong to the chair, so a profile cannot change them:
    // a profile saved with the IDs the other way round would mirror the steering.
    ConfigField<uint8_t> leftVesc(registry(), 60,
                                  {motor, "Left VESC ID", "CAN address of the left motor controller. The two addresses must differ.", "", false, true}, 2, 0, 253);
    ConfigField<uint8_t> rightVesc(registry(), 61,
                                   {motor, "Right VESC ID", "CAN address of the right motor controller. The two addresses must differ.", "", false, true}, 26, 0, 253);
    ConfigField<float> wheelCircumference(registry(), 62,
                                          {motor, "Wheel circumference", "Measured rolling circumference. Updates LINEAR_TO_RPM for wheel speed and acceleration conversion.", "m", false, true},
                                          common::WHEEL_CIRCUMFERENCE_M, 1.80f, 2.00f);
    // Distance between the two drive wheels where they touch the floor. It
    // converts a turn rate into a wheel-speed difference. The default is an
    // unmeasured estimate (common::TRACK_WIDTH_ESTIMATE_M): measure it on the
    // chair (cambered wheels are wider at the floor).
    ConfigField<float> trackWidth(registry(), 63,
                                  {motor, "Track width", "Distance between the drive wheels' contact patches. Converts turn rate (deg/s) to wheel speeds: too small a value makes the chair turn slower than the setting.", "m", false, true},
                                  common::TRACK_WIDTH_ESTIMATE_M, 0.30f, 1.20f);

    // The VESCs' own setting, repeated here so the firmware knows where their
    // speed mode stops working. It is the main cause of "nothing, then
    // everything": with the default, a wheel command under 15.5 RPM (0.49 m/s)
    // is not driven. Lower s_pid_min_erpm in VESC Tool on both VESCs, check on
    // the bench that a 5 RPM command is followed, then set this to match.
    ConfigField<float> vescMinSpeed(registry(), 64,
                                    {motor, "VESC minimum speed", "Wheel speed below which the VESCs will not drive in speed mode: their s_pid_min_erpm setting divided by 58.2. Keep it equal to what VESC Tool says. Stops switch to brake current just above it. 0 means the VESCs hold speed down to a standstill.", "RPM", false, true},
                                    15.5f, 0.0f, 30.0f);

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

    // A lean response curve is |input|^exponent, with the exponent set by the
    // knee. Below 1 the curve is steepest at neutral, and far below 1 any lean
    // past the deadband is nearly full speed. Above 4 almost nothing happens
    // until the very end of the lean. Both are kept out.
    static bool validLeanCurve(const float (&knee)[2])
    {
        const float exponent = logf(knee[1]) / logf(knee[0]);
        return exponent >= LeanCurveExponentMin && exponent <= LeanCurveExponentMax;
    }

    static bool validDriveSettings()
    {
        return leftVesc.get() != rightVesc.get() &&
               validLeanCurve(leanForewardBackResponse.get()) && validLeanCurve(leanSideToSideResponse.get());
    }

    static void repairDriveSettings()
    {
        // Each saved CAN ID may be individually valid but conflict with the other.
        // Repair this pair after load so unrelated migrated settings are preserved.
        if (leftVesc.get() == rightVesc.get())
        {
            leftVesc.reset();
            rightVesc.reset();
        }
        if (!validLeanCurve(leanForewardBackResponse.get()))
            leanForewardBackResponse.reset();
        if (!validLeanCurve(leanSideToSideResponse.get()))
            leanSideToSideResponse.reset();
    }

    // Persistence repairs migrated values on load. Protocol uses the same invariant
    // to reject conflicting live edits and rolls them back before acknowledging.
    Persistence persistence(registry(), eeprom, repairDriveSettings);
    Protocol protocol(registry(), persistence, validDriveSettings);

} // namespace settings
