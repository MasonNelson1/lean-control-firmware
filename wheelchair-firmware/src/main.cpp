#include <Arduino.h>
#include <ezButton.h>
#include "vesc_can.h"
#include "common.h"
#include "DriveController.h"
#include "PacketSerial.h"
#include "config/DriveSettings.h"
#include "drive_knobs.h"
#include "telemetry/Telemetry.h"
#include "watchdog.h"

using namespace common;

// Runtime knob selections, set by updateKnobs(). Start at the gentlest
// position until the knobs are read in setup().
knobs::RotarySwitch speedKnob;
knobs::RotarySwitch profileKnob;
static int lastProfilePosition = knobs::Invalid;
// The knob position the current settings belong to: the position at power-on,
// then the position of each profile confirmation.
static int confirmedProfilePosition = knobs::Invalid;
static int pendingProfile = knobs::Invalid;
static int activeProfile = knobs::Invalid;
float userMaxSpeedMS = knobs::speedLimitMS(0);

// MISC CONSTANTS and vars
ezButton calibrateButton(PIN_CAL_BUTTON);
ezButton leanModeButton(PIN_LEAN_BUTTON);
ezButton remoteModeButton(PIN_REMOTE_BUTTON);
ezButton disarmModeButton(PIN_DISARM_BUTTON);

VescBus can1;

// Cached motor addressing.
struct MotorSetup
{
    uint8_t leftId;
    uint8_t rightId;
};

static MotorSetup motors;
static VescStatus measuredMotors[2]{};
// Speed frames per second from each VESC, measured over the last second.
static uint32_t vescRateHz[2]{};

static telemetry::Telemetry telemetryOutput;

// Modes, faults, lean/remote/assist, smoothing and stopping all live here.
DriveController drive;

PacketSerial packetSerial;

static void onBluetoothPacket(const uint8_t *buffer, size_t size)
{
    // PacketSerial calls this with decoded bytes. Configuration packets enter
    // their own request slot, and ordinary remote packets keep their handler.
    if (!settings::protocol.queue(buffer, size))
        drive.onRemotePacket(buffer, size, millis());
}

static void applyRuntimeSettings()
{
    // Refresh values cached outside the registry after a successful mutation.
    LINEAR_TO_RPM = 60.0f / settings::wheelCircumference.get();
    motors = {settings::leftVesc.get(), settings::rightVesc.get()};
    calibrateButton.setDebounceTime(settings::buttonDebounce.get());
    leanModeButton.setDebounceTime(settings::buttonDebounce.get());
    remoteModeButton.setDebounceTime(settings::buttonDebounce.get());
    disarmModeButton.setDebounceTime(settings::buttonDebounce.get());
    drive.applySettings();
}

// Interrupts latch raw presses; ezButton confirms the debounced press in loop().
// Never call ezButton, print, or command motors from an interrupt handler.
static volatile bool leanEdgePending = false;
static volatile bool remoteEdgePending = false;
static volatile bool disarmEdgePending = false;

void onLeanButtonPress()
{
    leanEdgePending = true;
}

void onRemoteButtonPress()
{
    remoteEdgePending = true;
}

void onDisarmButtonPress()
{
    disarmEdgePending = true;
}
// Apply the knobs' current positions and report them as one text line.
// Non-CSV lines show up as console text in the debug viewer.
static void applyKnobs()
{
    userMaxSpeedMS = knobs::speedLimitMS(speedKnob.position());

    // this only prints when the knobs are moved, not all the time.
    telemetry::Telemetry::logf(
        "Knobs: speed %d/%d (%.2f m/s, raw %u)%s, profile %d/%d (raw %u)%s\n",
        speedKnob.position() + 1, knobs::Positions, userMaxSpeedMS,
        static_cast<unsigned>(speedKnob.lastRaw()), speedKnob.faulted() ? " FAULT->slowest" : "",
        profileKnob.position() + 1, knobs::Positions,
        static_cast<unsigned>(profileKnob.lastRaw()), profileKnob.faulted() ? " FAULT" : "");
}

static bool activateProfile(int slot)
{
    if (!settings::protocol.selectProfile(slot))
        return false;
    activeProfile = slot;
    applyRuntimeSettings();
    drive.resetForProfile();
    for (auto &motor : measuredMotors)
        motor = {};
    return true;
}

static void updateProfileLED()
{
    constexpr uint32_t pulseMs = 200;
    constexpr uint32_t gapMs = 2000;
    static bool wasSelecting = false;
    static int displayedProfile = knobs::Invalid;
    static uint32_t cycleStart = 0;
    const uint32_t now = millis();
    const bool selecting = pendingProfile != knobs::Invalid;
    if (displayedProfile != activeProfile || (wasSelecting && !selecting))
    {
        displayedProfile = activeProfile;
        cycleStart = now;
    }
    wasSelecting = selecting;

    bool ledOn = selecting;
    if (!selecting && activeProfile != knobs::Invalid)
    {
        const uint32_t burstMs = (2 * (activeProfile + 1) - 1) * pulseMs;
        const uint32_t phase = (now - cycleStart) % (burstMs + gapMs);
        cycleStart = now - phase;
        ledOn = phase < burstMs && phase % (2 * pulseMs) < pulseMs;
    }
    digitalWrite(PIN_CONNECTED_LED, ledOn ? HIGH : LOW);
}

static void updateKnobs()
{
    static uint32_t lastSampleMs = 0;
    const uint32_t now = millis();
    if (now - lastSampleMs < knobs::SampleMs)
        return;
    lastSampleMs = now;

    const bool speedChanged = speedKnob.update(analogRead(PIN_SPEED), now);
    const bool profileChanged = profileKnob.update(analogRead(PIN_ACCEL), now);
    if (profileKnob.settled() && profileKnob.position() != lastProfilePosition)
    {
        lastProfilePosition = profileKnob.position();
        // A pending profile reserves the LEAN button until it is confirmed.
        // Turning the knob back to where it was cancels that, so a knob that
        // was only bumped does not leave the LEAN button dead.
        pendingProfile = lastProfilePosition == confirmedProfilePosition ? knobs::Invalid : lastProfilePosition;
    }
    if (speedChanged || profileChanged)
        applyKnobs();
}
void setup()
{
    static uint8_t bluetoothTxBuffer[512];
    const bool watchdogReset = watchdog::causedLastReset();

    // set up serial and telemetry stuff
    Serial.begin(115200);
    Serial1.begin(115200);
    Serial1.addMemoryForWrite(bluetoothTxBuffer, sizeof(bluetoothTxBuffer));
    packetSerial.setStream(&Serial1);
    packetSerial.setPacketHandler(onBluetoothPacket);

    // setup led's
    pinMode(common::PIN_CAL_LED, OUTPUT);
    pinMode(common::PIN_REMOTE_LED, OUTPUT);
    pinMode(common::PIN_LEAN_LED, OUTPUT);
    pinMode(common::PIN_DISARM_LED, OUTPUT);
    pinMode(common::PIN_CONNECTED_LED, OUTPUT);

    settings::persistence.load();
    drive.begin(watchdogReset);
    applyRuntimeSettings();

    // user input
    analogReadResolution(12);
    analogReadAveraging(4);
    // Take the knobs' power-on positions before the first drive tick. An
    // invalid reading leaves that knob at its slowest position.
    speedKnob.seed(analogRead(PIN_SPEED), millis());
    const auto profileRaw = analogRead(PIN_ACCEL);
    profileKnob.seed(profileRaw, millis());
    lastProfilePosition = confirmedProfilePosition = knobs::classify(profileRaw);
    if (lastProfilePosition != knobs::Invalid)
        activateProfile(lastProfilePosition);
    applyKnobs();
    // ezButton configures the pins as INPUT_PULLUP: pressing connects to GND.

    attachInterrupt(digitalPinToInterrupt(PIN_LEAN_BUTTON), onLeanButtonPress, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_REMOTE_BUTTON), onRemoteButtonPress, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_DISARM_BUTTON), onDisarmButtonPress, FALLING);

    // motors
    vescBegin(can1);
    if (watchdogReset)
        telemetry::Telemetry::logf("Restarted by the watchdog: the firmware had stopped responding.\n");
    if (!settings::persistence.loadedSaved())
        telemetry::Telemetry::logf("No valid saved settings were found: running on firmware defaults. "
                                   "Tare the sensors and run the range calibration again.\n");
    telemetry::Telemetry::logf("Mode: %s\n", modeName(drive.mode()));
    drive.lean().publishRanges();
    telemetry::Telemetry::logf("Setup successful, ready to drive.\n");

    // Last: everything above may take longer than the watchdog allows.
    settings::storageActivity = watchdog::feed;
    watchdog::begin();
}

// Hardware adapter: the state machine owns all pending presses and timers.
static void updateModeInputs()
{
    ModeStateMachine::Inputs input;
    noInterrupts();
    input.lean.edge = leanEdgePending;
    input.remote.edge = remoteEdgePending;
    input.disarm.edge = disarmEdgePending;
    leanEdgePending = remoteEdgePending = disarmEdgePending = false;
    interrupts();

    leanModeButton.loop();
    remoteModeButton.loop();
    calibrateButton.loop();
    disarmModeButton.loop();
    input.lean.pressed = leanModeButton.isPressed();
    input.lean.released = leanModeButton.isReleased();
    input.remote.pressed = remoteModeButton.isPressed();
    input.remote.released = remoteModeButton.isReleased();
    input.disarm.pressed = disarmModeButton.isPressed();
    input.disarm.released = disarmModeButton.isReleased();
    input.calibrate.pressed = calibrateButton.isPressed();
    input.calibrate.released = calibrateButton.isReleased();
    input.leanHeld = leanModeButton.getStateRaw() == LOW;
    input.profileSettled = profileKnob.settled();
    input.pendingProfile = pendingProfile;
    // The controller fills in the seat and calibration state.
    const auto actions = drive.updateButtons(input, millis());
    if (actions.activateProfile != ModeStateMachine::NoProfile)
    {
        if (!activateProfile(actions.activateProfile))
            telemetry::Telemetry::logf("Profile %d unavailable; keeping current settings.\n", actions.activateProfile + 1);
        confirmedProfilePosition = actions.activateProfile;
        pendingProfile = knobs::Invalid;
    }
}

static void updateModeLEDs()
{
    const uint32_t now = millis();
    const mode current = drive.mode();
    digitalWrite(common::PIN_LEAN_LED, current == LEAN ? HIGH : LOW);
    digitalWrite(common::PIN_REMOTE_LED, current == REMOTE ? HIGH : LOW);
    // DISARM: solid; a fast blink when a fault put the chair there (it stays
    // until a mode is armed again); a slow wink off while the empty chair is
    // free-wheeling. Assist: slow blink.
    const bool faulted = drive.fault() != DriveController::Fault::None;
    const bool disarmLED = current == DISARM ? (faulted ? (now / 100) % 2 == 0
                                                        : !drive.freewheeling() || (now / 100) % 10 != 0)
                                             : current == ASSIST && drive.modeState().assistLEDOn(now);
    digitalWrite(common::PIN_DISARM_LED, disarmLED ? HIGH : LOW);
    // Calibration LED: lean calibration in LEAN, the sensor tare in DISARM.
    const bool calLED = current == LEAN || (current == DISARM && drive.lean().tareActive());
    digitalWrite(common::PIN_CAL_LED, calLED && drive.lean().calibrationLEDOn(now) ? HIGH : LOW);
}

static void updateDrive()
{
    // rate limit the loop.
    // this is placed right before the motor communication and pid smoothing because pid needs to run at a fixed rate.
    static uint32_t tSend = 0;
    const uint32_t now = millis();
    const uint32_t period = settings::controlPeriod.get();
    const uint32_t elapsed = now - tSend;
    if (elapsed < period)
    {
        return;
    }
    tSend = now;

    DriveController::TickInput input;
    input.now = now;
    // The real time since the last tick, so the filters and ramps stay right
    // when a loop runs late. Capped: after a long stall (an EEPROM save) one
    // tick must not take one huge step.
    input.dt = min(elapsed, 4 * period) / 1000.0f;
    input.leftRPM = vescWheelRpm(measuredMotors[0]);
    input.rightRPM = vescWheelRpm(measuredMotors[1]);
    input.leftAmps = measuredMotors[0].current;
    input.rightAmps = measuredMotors[1].current;
    input.leftFresh = vescFresh(measuredMotors[0], now, VESC_STALE_MS);
    input.rightFresh = vescFresh(measuredMotors[1], now, VESC_STALE_MS);
    input.speedLimitMS = userMaxSpeedMS;
    input.calButtonHeld = calibrateButton.getState() == LOW;

    const auto command = drive.tick(input);
    const auto send = [](uint8_t id, const DriveController::MotorCommand::Wheel &wheel) {
        switch (wheel.kind)
        {
        case DriveController::MotorCommand::Kind::RPM:
            vescSetWheelRPM(can1, id, wheel.value);
            break;
        case DriveController::MotorCommand::Kind::Brake:
            vescSetBrakeCurrent(can1, id, wheel.value);
            break;
        default:
            vescSetCurrent(can1, id, wheel.value);
            break;
        }
    };
    send(motors.leftId, command.left);
    send(motors.rightId, command.right);
}

static void replyToConfiguration()
{
    constexpr size_t MaxPacketBytes = config::MaxResponseSize + config::MaxResponseSize / 254 + 3;
    // Settings can change only in DISARM, and not while the chair is still
    // braking to a stop there: an EEPROM save would stall the stop.
    // Nor while the wheels are turning (a rider pushing by hand in DISARM): no
    // commands go out during a save, and the VESCs would time out and brake.
    const uint32_t now = millis();
    const bool rolling = vescFresh(measuredMotors[0], now, VESC_STALE_MS) && vescFresh(measuredMotors[1], now, VESC_STALE_MS) &&
                         max(fabsf(vescWheelRpm(measuredMotors[0])), fabsf(vescWheelRpm(measuredMotors[1]))) >= STOP_DONE_RPM;
    const bool disarmed = drive.mode() == DISARM && !drive.stopping() && !rolling;
    settings::protocol.tick(millis(), disarmed);
    if (!settings::protocol.pending() || Serial1.availableForWrite() < static_cast<int>(MaxPacketBytes))
        return;

    uint8_t response[config::MaxResponseSize];
    // Configuration writes are permitted only while the current mode is DISARM.
    // Only successful runtime mutations require refreshing derived settings.
    const uint32_t revision = settings::protocol.revision();
    const size_t length = settings::protocol.reply(response, disarmed);
    if (settings::protocol.revision() != revision)
        applyRuntimeSettings();
    // The leading zero distinguishes PacketSerial responses from legacy CSV.
    Serial1.write(static_cast<uint8_t>(0));
    packetSerial.send(response, length);
}

// Once a second: lean ranges and a health summary, as text lines the dashboard reads.
static void publishStatus(uint32_t now)
{
    static uint32_t lastStatusMs = 0;
    static uint32_t lastFrames[2]{};
    if (now - lastStatusMs < 1000)
        return;
    const uint32_t elapsed = now - lastStatusMs;
    lastStatusMs = now;
    for (int i = 0; i < 2; ++i)
    {
        // A profile load clears the counters; treat that as no frames.
        const uint32_t frames = measuredMotors[i].rpmFrames;
        vescRateHz[i] = frames >= lastFrames[i] ? (frames - lastFrames[i]) * 1000 / elapsed : 0;
        lastFrames[i] = frames;
    }
    drive.lean().publishRanges();
    // saved=0: the settings are firmware defaults, not a saved bank (see setup()).
    telemetry::Telemetry::logf("HEALTH fault=%s seated=%d load=%d vescL=%lu vescR=%lu stop=%d cal=%d saved=%d\n",
                               DriveController::faultName(drive.fault()), drive.lean().isSeated() ? 1 : 0,
                               drive.lean().totalLoad(), static_cast<unsigned long>(vescRateHz[0]),
                               static_cast<unsigned long>(vescRateHz[1]), drive.stopping() ? 1 : 0,
                               drive.lean().calibrated() ? 1 : 0, settings::persistence.loadedSaved() ? 1 : 0);
}

static void publishTelemetry()
{
    const uint32_t now = millis();
    publishStatus(now);
    if (!telemetryOutput.due(now))
        return;
    telemetry::TelemetryFrame frame;
    frame.timeMs = now;
    frame.mode = drive.mode();
    frame.lean = drive.lean().getTelemetry();
    frame.userWeight = drive.lean().getUserWeightEstimate();
    frame.remoteStopped = drive.remoteStopped();
    frame.commands = drive.commands();
    const auto feedback = [](const VescStatus &motor)
    {
        return telemetry::MotorFeedback{vescWheelRpm(motor), motor.lastRpmMs, motor.rpmFrames != 0};
    };
    frame.left = feedback(measuredMotors[0]);
    frame.right = feedback(measuredMotors[1]);
    frame.boostCurrent = drive.assistCurrent();
    telemetryOutput.publish(frame);
}

void loop()
{
    watchdog::feed();
    updateKnobs();
    updateModeInputs();
    packetSerial.update();
    CAN_message_t feedback;
    for (int i = 0; i < 32 && can1.read(feedback); ++i)
    {
        vescHandleFrame(feedback, motors.leftId, measuredMotors[0]);
        vescHandleFrame(feedback, motors.rightId, measuredMotors[1]);
    }
    updateDrive();
    updateModeLEDs();
    updateProfileLED();
    replyToConfiguration();

    publishTelemetry();
}
