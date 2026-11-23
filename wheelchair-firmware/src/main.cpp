#include <Arduino.h>
#include <ezButton.h>
#include "vesc_can.h"
#include "vesc_probe.h"
#include "vesc_apply.h"
#include "vesc_monitor.h"
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

// Timing and link counters for the once-a-second DBG SYS line (telemetry only).
static struct
{
    uint32_t loops = 0, loopMaxUs = 0;        // since the last DBG SYS
    uint32_t ctrlMaxMs = 0, ctrlMinMs = ~0u;  // control tick spacing since the last DBG SYS
    uint32_t canRx = 0;                       // CAN frames read since boot
} sysStats;
static uint32_t configReplyBytes = 0; // configuration replies sent over Bluetooth since boot

static void noteControlTick(uint32_t gapMs)
{
    sysStats.ctrlMaxMs = max(sysStats.ctrlMaxMs, gapMs);
    sysStats.ctrlMinMs = min(sysStats.ctrlMinMs, gapMs);
}

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
    telemetry::Telemetry::logf("DBG EVT %lu profile slot=%d\n", static_cast<unsigned long>(millis()), slot + 1);
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
    // Once-a-second lines each have their own 20 Hz tick (publishTelemetry), so
    // the largest single tick is a 20 Hz group (CSV ≈95, DBG R ≈30, COP ≈35,
    // M ≈75) plus DBG VVAL ≈260: ≈ 500 bytes. A 50 ms tick drains ≈ 576 bytes
    // at 115200 baud. Telemetry drops whole lines rather than block when this is
    // full and counts them (DBG SYS drop=).
    static uint8_t bluetoothTxBuffer[1024];
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
        telemetry::Telemetry::logf("No valid saved settings were found: running on firmware defaults.\n");
    telemetry::Telemetry::logf("Mode: %s\n", modeName(drive.mode()));
    drive.lean().publishDynamicGains();
    telemetry::Telemetry::logf("Setup successful, ready to drive.\n");

    // Last: everything above may take longer than the watchdog allows.
    settings::storageActivity = watchdog::feed;
    watchdog::begin();
}

// DBG EVT for every debounced press and release, accepted by the mode state
// machine or not (a press with the seat empty is discarded there).
static void logButtonEvents(const ModeStateMachine::Inputs &input)
{
    const struct
    {
        const char *name;
        const ModeStateMachine::Button &button;
    } buttons[] = {{"lean", input.lean}, {"remote", input.remote}, {"disarm", input.disarm}, {"cal", input.calibrate}};
    for (const auto &b : buttons)
        if (b.button.pressed || b.button.released)
            telemetry::Telemetry::logf("DBG EVT %lu btn %s %s\n", static_cast<unsigned long>(millis()), b.name,
                                       b.button.pressed ? "down" : "up");
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
    logButtonEvents(input);
    const mode selectedBefore = drive.modeState().selected();
    // The controller fills in the seat and calibration state.
    const auto actions = drive.updateButtons(input, millis());
    if (drive.modeState().selected() != selectedBefore)
        telemetry::Telemetry::logf("DBG EVT %lu select %s->%s cur=%s seated=%d\n", static_cast<unsigned long>(millis()),
                                   modeName(selectedBefore), modeName(drive.modeState().selected()),
                                   modeName(drive.mode()), drive.lean().isSeated() ? 1 : 0);
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
    // Calibration LED: lean calibration in LEAN (on until calibrated, fast
    // blink while collecting the reach, slow blink in the relaxed-press window).
    digitalWrite(common::PIN_CAL_LED, current == LEAN && drive.lean().calibrationLEDOn(now) ? HIGH : LOW);
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
    noteControlTick(elapsed);
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
    {
        applyRuntimeSettings();
        telemetry::Telemetry::logf("DBG EVT %lu settings rev=%lu\n", static_cast<unsigned long>(millis()),
                                   static_cast<unsigned long>(settings::protocol.revision()));
    }
    // The leading zero distinguishes PacketSerial responses from legacy CSV.
    Serial1.write(static_cast<uint8_t>(0));
    packetSerial.send(response, length);
    configReplyBytes += length + length / 254 + 3; // COBS-framed, approximately
}

// Read-only VESC firmware-version probe (see vesc_probe.h).
static vescprobe::Reply probeReply;
static vescapply::Applier vescApplier; // one-shot approved VESC settings write (vesc_apply.h)
static vescmon::Monitor vescMonitor;   // read-only GET_VALUES poll + fault watch (vesc_monitor.h)
static uint32_t probeAsked = 0, probeFrames = 0, probeReplies = 0;
// A config reply being streamed out as hex, a chunk per telemetry tick.
static uint8_t confDump[1024];
static uint16_t confDumpLen = 0, confDumpOff = 0;
static uint8_t confDumpId = 0;

static void logProbeReply()
{
    const auto &r = probeReply;
    if (r.len >= 3 && r.data[0] == vescprobe::COMM_FW_VERSION)
    {
        char hw[33] = {0};
        for (uint16_t i = 3, j = 0; i < r.len && j < sizeof(hw) - 1 && r.data[i]; ++i, ++j)
            hw[j] = static_cast<char>(r.data[i]);
        telemetry::Telemetry::logf("DBG VESCFW id=%u fw=%u.%02u hw=%s len=%u crc=%d\n", r.fromId, r.data[1], r.data[2], hw, r.len, r.crcOk);
    }
    else if (r.crcOk && r.len > 0 && (r.data[0] == vescprobe::COMM_GET_MCCONF || r.data[0] == vescprobe::COMM_GET_APPCONF) &&
             confDumpOff >= confDumpLen && r.len <= sizeof(confDump))
    {
        memcpy(confDump, r.data, r.len);
        confDumpLen = r.len;
        confDumpOff = 0;
        confDumpId = r.fromId;
    }
    else
        telemetry::Telemetry::logf("DBG VESCFW id=%u cmd=%u len=%u crc=%d\n", r.fromId, r.len ? r.data[0] : 255, r.len, r.crcOk);
}

// One 24-byte hex chunk of the pending config dump (decoded by tools/vesc_conf.py).
static void streamConfDump()
{
    if (confDumpOff >= confDumpLen)
        return;
    char hex[49];
    const uint16_t n = min<uint16_t>(24, confDumpLen - confDumpOff);
    for (uint16_t i = 0; i < n; ++i)
        snprintf(hex + 2 * i, 3, "%02x", confDump[confDumpOff + i]);
    hex[2 * n] = 0;
    telemetry::Telemetry::logf("DBG VCONF id=%u cmd=%u len=%u off=%u %s\n", confDumpId, confDump[0], confDumpLen, confDumpOff, hex);
    confDumpOff += n;
}

// 20 Hz: what each VESC reports and what the motion smoother is doing.
//   DBG M ms curL curR dutyL dutyR iinL iinR pidL pidR accL accR rateL rateR intL intR limit
// cur/duty/iin from STATUS_1/STATUS_4 (A, A). Same columns as on dev-setup,
// but this build smooths forward speed and turn on separate axes
// (MotionSmoother), so: pidL/pidR = the smoother's wheel commands
// (linear ± turn) before DriveController's clamp to measured ± 50 RPM
// (the CSV's smoothed RPM is after it); acc, rate (RPM/s) and int (RPM·s) are
// the forward-speed smoother's and repeat in the L and R columns; limit =
// slewed speed cap m/s.
static void publishMotorLine(uint32_t now)
{
    const VescStatus &l = measuredMotors[0], &r = measuredMotors[1];
    const MotionSmoother &m = drive.motion();
    const RPMSetpointPID &p = m.linear();
    telemetry::Telemetry::logf("DBG M %lu %.1f %.1f %.3f %.3f %.1f %.1f %.1f %.1f %.0f %.0f %.0f %.0f %.1f %.1f %.2f\n",
                               static_cast<unsigned long>(now), l.current, r.current, l.duty, r.duty,
                               l.inputCurrent, r.inputCurrent, m.linearRPM() + m.turnRPM(), m.linearRPM() - m.turnRPM(),
                               p.acceleration(), p.acceleration(), p.filteredRate(), p.filteredRate(), p.integral(),
                               p.integral(), drive.speedLimitMS());
}
// 1 Hz: the Teensy itself. loopMax µs and loops since the last line; ctrl =
// control tick spacing min/max ms; cpuC = die temperature; canRx frames and
// canTxFail refused command frames since boot; tec/rec = CAN error counters;
// bt = Bluetooth bytes/lines written since boot, drop = bytes/lines dropped
// for lack of TX space, btFree = least TX space seen this second, cfg =
// configuration reply bytes.
static void publishSystemLine(uint32_t now)
{
    const auto &link = telemetry::Telemetry::link();
    const uint32_t ecr = FLEXCAN1_ECR; // read-only error counters
    telemetry::Telemetry::logf("DBG SYS up=%lu loops=%lu loopMax=%lu ctrl=%lu/%lu cpuC=%.1f canRx=%lu canTxFail=%lu "
                               "tec=%lu rec=%lu bt=%lu/%lu drop=%lu/%lu btFree=%d cfg=%lu\n",
                               static_cast<unsigned long>(now / 1000), static_cast<unsigned long>(sysStats.loops),
                               static_cast<unsigned long>(sysStats.loopMaxUs),
                               static_cast<unsigned long>(sysStats.ctrlMinMs == ~0u ? 0 : sysStats.ctrlMinMs),
                               static_cast<unsigned long>(sysStats.ctrlMaxMs), tempmonGetTemp(),
                               static_cast<unsigned long>(sysStats.canRx), static_cast<unsigned long>(vescTxFailures()),
                               static_cast<unsigned long>(ecr & 0xFF), static_cast<unsigned long>((ecr >> 8) & 0xFF),
                               static_cast<unsigned long>(link.bytes), static_cast<unsigned long>(link.lines),
                               static_cast<unsigned long>(link.droppedBytes), static_cast<unsigned long>(link.droppedLines),
                               telemetry::Telemetry::takeMinFree(), static_cast<unsigned long>(configReplyBytes));
    sysStats.loops = sysStats.loopMaxUs = sysStats.ctrlMaxMs = 0;
    sysStats.ctrlMinMs = ~0u;
}

// 1 Hz: knobs (position 1-4 / raw ADC / S settled, F faulted, - settling),
// buttons as raw levels (1 = held: lean remote disarm cal), the profile knob's
// pending and active slots (0 = none), selected and current mode, seat.
static void publishInputsLine()
{
    const auto knob = [](const knobs::RotarySwitch &k) { return k.faulted() ? 'F' : k.settled() ? 'S' : '-'; };
    const auto held = [](ezButton &b) { return b.getStateRaw() == LOW ? 1 : 0; };
    telemetry::Telemetry::logf("DBG IO spd=%d/%u/%c prof=%d/%u/%c btn=%d%d%d%d pend=%d act=%d sel=%s cur=%s seated=%d\n",
                               speedKnob.position() + 1, static_cast<unsigned>(speedKnob.lastRaw()), knob(speedKnob),
                               profileKnob.position() + 1, static_cast<unsigned>(profileKnob.lastRaw()), knob(profileKnob),
                               held(leanModeButton), held(remoteModeButton), held(disarmModeButton), held(calibrateButton),
                               pendingProfile + 1, activeProfile + 1, modeName(drive.modeState().selected()),
                               modeName(drive.mode()), drive.lean().isSeated() ? 1 : 0);
}

// Once-a-second lines go out on their own 20 Hz tick (slot = tick % 20) so no
// tick sends more than ≈ 500 bytes. Sent together (≈ 700 bytes in one burst)
// the HC-06 lost the tail of the burst: in the 2026-11-22 tap the last line of
// it (DBG VESC) arrived intact only 82-88% of the time against ≈ 99.6% for the
// 1 Hz: the drive controller's health summary (council/firmware-9's HEALTH line).
// vescL/vescR = speed frames per second from each VESC over the last second.
static void publishHealthLine(uint32_t now)
{
    static uint32_t lastStatusMs = 0;
    static uint32_t lastFrames[2]{};
    const uint32_t elapsed = now - lastStatusMs;
    lastStatusMs = now;
    for (int i = 0; i < 2; ++i)
    {
        // A profile load clears the counters; treat that as no frames.
        const uint32_t frames = measuredMotors[i].rpmFrames;
        vescRateHz[i] = frames >= lastFrames[i] && elapsed ? (frames - lastFrames[i]) * 1000 / elapsed : 0;
        lastFrames[i] = frames;
    }
    // saved=0: the settings are firmware defaults, not a saved bank (see setup()).
    telemetry::Telemetry::logf("HEALTH fault=%s seated=%d load=%d vescL=%lu vescR=%lu stop=%d cal=%d saved=%d\n",
                               DriveController::faultName(drive.fault()), drive.lean().isSeated() ? 1 : 0,
                               drive.lean().totalLoad(), static_cast<unsigned long>(vescRateHz[0]),
                               static_cast<unsigned long>(vescRateHz[1]), drive.stopping() ? 1 : 0,
                               drive.lean().calibrated() ? 1 : 0, settings::persistence.loadedSaved() ? 1 : 0);
}

// Once-a-second lines go out on their own 20 Hz tick (slot = tick % 20) so no
// tick sends more than ≈ 500 bytes. Sent together (≈ 700 bytes in one burst)
// the HC-06 lost the tail of the burst: in the 2026-11-22 tap the last line of
// it (DBG VESC) arrived intact only 82-88% of the time against ≈ 99.6% for the
// 20 Hz lines. The config dump streams on odd ticks, which carry no slot line.
static void publishTelemetry()
{
    const uint32_t now = millis();
    if (!telemetryOutput.due(now))
        return;
    static uint32_t tick = 0;
    const uint32_t slot = tick++ % 20;
    switch (slot)
    {
    case 0:
        drive.lean().publishDynamicGains();
        drive.lean().publishSensorStatus();
        break;
    case 2:
        drive.lean().publishCornerStatus();
        break;
    case 4:
    {
        // Every 5 s one read-only GET to one VESC, cycling: firmware version,
        // motor config, app config; left then right. A full cycle takes 30 s.
        // The monitor sends it when no other reply is in flight.
        static uint8_t probeTick = 0;
        if (++probeTick % 5 == 0 && !vescApplier.active())
        {
            static const uint8_t comms[3] = {vescprobe::COMM_FW_VERSION, vescprobe::COMM_GET_MCCONF, vescprobe::COMM_GET_APPCONF};
            const uint8_t probeSlot = (probeTick / 5) % 6;
            vescMonitor.queueProbe(probeSlot < 3 ? motors.leftId : motors.rightId, comms[probeSlot % 3]);
            ++probeAsked;
        }
        telemetry::Telemetry::logf("DBG VESCPROBE asked=%lu frames=%lu replies=%lu\n", static_cast<unsigned long>(probeAsked),
                                   static_cast<unsigned long>(probeFrames), static_cast<unsigned long>(probeReplies));
        publishSystemLine(now);
        break;
    }
    case 6:
    case 8:
        // Motor controller health from the CAN status frames (vesc_monitor.h).
        vescmon::logStatus(measuredMotors[slot == 6 ? 0 : 1], slot == 6 ? motors.leftId : motors.rightId,
                           slot == 6 ? 'L' : 'R', now);
        break;
    case 10:
    case 12:
        vescMonitor.logValues(slot == 10 ? 0 : 1, now); // COMM_GET_VALUES, fault code included
        break;
    case 14:
        publishInputsLine();
        break;
    case 16:
        publishHealthLine(now);
        break;
    default:
        if (slot % 2)
            streamConfDump();
        break;
    }
    // DriveController samples the seat sensors on every control tick in every mode.
    // Corner layout: re-zero the empty-plate baseline (RAM only) while disarmed
    // with nobody seated. Never runs in LEAN, and entering LEAN forces a new
    // neutral calibration, so a baseline never changes under a live neutral.
    drive.trackEmptyPlate(now);
    {
        // 20 Hz raw ADC counts (front/back/left/right) for lean-transition analysis.
        const auto &raw = drive.lean().getRawReadings();
        telemetry::Telemetry::logf("DBG R %lu %d %d %d %d\n", static_cast<unsigned long>(now), raw[0], raw[1], raw[2], raw[3]);
        // 20 Hz centre of pressure in inches (corner layout only).
        drive.lean().publishCentreOfPressure(now);
    }
    publishMotorLine(now);
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
    static uint32_t loopStartUs = micros();
    const uint32_t loopUs = micros() - loopStartUs; // the previous loop, start to start
    loopStartUs += loopUs;
    sysStats.loopMaxUs = max(sysStats.loopMaxUs, loopUs);
    ++sysStats.loops;

    watchdog::feed();
    updateKnobs();
    updateModeInputs();
    packetSerial.update();
    CAN_message_t feedback;
    for (int i = 0; i < 32 && can1.read(feedback); ++i)
    {
        ++sysStats.canRx;
        vescHandleFrame(feedback, motors.leftId, measuredMotors[0]);
        vescHandleFrame(feedback, motors.rightId, measuredMotors[1]);
        if (feedback.flags.extended && (feedback.id & 0xFF) == vescprobe::PROBE_CAN_ID)
            ++probeFrames;
        if (vescprobe::handleFrame(feedback, probeReply))
        {
            ++probeReplies;
            if (vescApplier.active())
                vescApplier.onReply(probeReply);
            else if (!vescMonitor.onReply(probeReply, millis()))
                logProbeReply();
            probeReply.done = false;
        }
    }
    vescMonitor.flushFaults(); // a DBG VFAULT goes out as soon as there is room
    {
        const bool disarmed = drive.mode() == DISARM;
        // The applier starts only when no monitor/probe reply is in flight; while it
        // runs, the monitor asks nothing (both share the reply path to PROBE_CAN_ID).
        // (VESC_APPLY_ENABLED is false in this build: maybeStart returns at once.)
        vescApplier.maybeStart(vescFresh(measuredMotors[0], millis(), 500) && vescFresh(measuredMotors[1], millis(), 500) &&
                                   !vescMonitor.busy(millis()),
                               disarmed, motors.leftId, motors.rightId);
        vescApplier.update(can1, disarmed);
        vescApplier.repeatSummary();
        vescMonitor.update(can1, millis(), motors.leftId, motors.rightId, !vescApplier.active());
    }
    updateDrive();
    updateModeLEDs();
    updateProfileLED();
    replyToConfiguration();

    publishTelemetry();
}
