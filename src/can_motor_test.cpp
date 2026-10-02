#include <Arduino.h>
#include <FlexCAN_T4.h>

// ============================================================
// LEAN — two-sensor CAN motor bench test (HANDOFF.md section 8)
// ------------------------------------------------------------
// Built by its own PlatformIO env:  pio run -e can_test
// The main lean pipeline (src/main.cpp) is not part of this build.
//
// STAGE 1 — listen only. No command frames are compiled in at all.
//           Reads the sensors, receives VESC status, prints at 10 Hz.
// STAGE 2 — typed duty. SET_DUTY sent to VESC_ID at 50 Hz continuously
//           (zeros included), from a duty typed into the Serial Monitor:
//             d5 -> duty +0.05    d-5 -> duty -0.05    d0 -> stop
// STAGE 3 — sensors in control:
//             sensor 1 (A16) only -> forward, harder = faster
//             sensor 2 (A17) only -> reverse
//             both or neither     -> 0
//
// In Stages 2 and 3:
//   s -> STOP NOW and latch (no Enter needed)
//   g -> re-arm after a stop
// Stage 3 also stays at 0 until both sensors are released: after boot,
// after 'g', after a sensor fault, and after VESC status is lost.
// A reading near 3.3 V (shorted sensor / wire on 3.3 V) is a fault -> 0.
//
// Note on "listen only": the CAN controller is in normal mode, not the
// FlexCAN LISTEN_ONLY mode. With one VESC on the bench, the Teensy is
// the only other node, so it must send the ACK bit or the VESC sees
// every frame as failed and keeps retransmitting. An ACK is not a command.
//
// Board: Teensy 4.1 | Serial Monitor baud: 115200
// ============================================================

#define STAGE 3

// ---------- PINS ----------
const uint8_t PIN_S1 = A16;   // pin 40 — sensor 1 (forward)
const uint8_t PIN_S2 = A17;   // pin 41 — sensor 2 (reverse)
// CAN1: TX = pin 22, RX = pin 23 (fixed by FlexCAN_T4 for CAN1)

// ---------- CAN / VESC ----------
const uint8_t  VESC_ID  = 1;          // left VESC — the only target for this test
const uint32_t CAN_BAUD = 500000;

// VESC packet types (id = controller_id | (type << 8), extended 29-bit)
const uint8_t PKT_SET_DUTY = 0;       // int32 duty * 100000
const uint8_t PKT_STATUS   = 9;       // ERPM, current, duty
const uint8_t PKT_STATUS_4 = 16;      // MOSFET temp, motor temp, input current
const uint8_t PKT_STATUS_5 = 27;      // tachometer, input voltage

const uint32_t STATUS_TIMEOUT_MS = 500;   // no status this long = CAN problem

// ---------- DUTY COMMAND (Stage 2+) ----------
const float    MAX_DUTY = 0.10f;      // hard cap for this test (keep <= 0.15)
const float    RAMP_S   = 0.5f;       // 0 -> MAX_DUTY takes at least this long
const uint32_t SEND_MS  = 20;         // 50 Hz, well inside the VESC timeout
const float    SLEW_PER_SEND = MAX_DUTY * (SEND_MS / 1000.0f) / RAMP_S;

// ---------- SENSORS ----------
const int FLOOR  = 370;               // no-load ADC reading (0.3 V bias)
const int THRESH = 60;                // counts above FLOOR before a press counts
const int FULL   = 300;               // counts above FLOOR for MAX_DUTY
const int RAIL   = 4000;              // raw reading this high = sensor fault

FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> can1;

// ---------- STATE (latest values from the VESC) ----------
int32_t  erpm = 0;
float    motorCurrent = 0.0f;         // A, from STATUS
float    vescDuty = 0.0f;             // duty the VESC reports it is running
float    mosfetTempC = 0.0f;
float    inputVoltage = 0.0f;
bool     gotStatus = false, gotStatus4 = false, gotStatus5 = false;
uint32_t framesRx = 0;                // status frames from VESC_ID
uint32_t lastStatusMs = 0;            // millis() of the last one
int      otherId = -1;                // a VESC id we heard that isn't VESC_ID
uint32_t lastOtherMs = 0;             // millis() of the last frame from otherId

// ---------- STATE (command) ----------
float targetDuty = 0.0f;              // typed (Stage 2) or from sensors (Stage 3)
float cmdDuty    = 0.0f;              // slew-limited, what is actually sent
bool  stopped    = false;             // 's' latch: forces 0 until 'g'
bool  armed      = false;             // Stage 3: sensors released since last reset
bool  sensorFault = false;            // Stage 3: a sensor reads near 3.3 V

// ---------- HELPERS ----------
// VESC sends multi-byte values big-endian, signed.
int32_t be32(const uint8_t *p) {
  return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8)  |  (uint32_t)p[3]);
}
int16_t be16(const uint8_t *p) {
  return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int aboveFloor(int raw) {
  int s = raw - FLOOR;
  return s < 0 ? 0 : s;
}

int readSensor(uint8_t pin) { return aboveFloor(analogRead(pin)); }

// Decode one received frame. Ignores anything that isn't a VESC status frame.
void handleFrame(const CAN_message_t &msg) {
  if (!msg.flags.extended) return;            // VESC uses 29-bit ids only
  uint8_t ctrlId = msg.id & 0xFF;
  uint8_t type   = (msg.id >> 8) & 0xFF;
  if (type != PKT_STATUS && type != PKT_STATUS_4 && type != PKT_STATUS_5) return;

  if (ctrlId != VESC_ID) { otherId = ctrlId; lastOtherMs = millis(); return; }

  framesRx++;
  lastStatusMs = millis();

  if (type == PKT_STATUS && msg.len >= 8) {
    erpm         = be32(&msg.buf[0]);
    motorCurrent = be16(&msg.buf[4]) / 10.0f;
    vescDuty     = be16(&msg.buf[6]) / 1000.0f;
    gotStatus = true;
  } else if (type == PKT_STATUS_4 && msg.len >= 2) {
    mosfetTempC = be16(&msg.buf[0]) / 10.0f;
    gotStatus4 = true;
  } else if (type == PKT_STATUS_5 && msg.len >= 6) {
    inputVoltage = be16(&msg.buf[4]) / 10.0f;
    gotStatus5 = true;
  }
}

#if STAGE >= 2
// SET_DUTY to VESC_ID: int32 duty * 100000, big-endian.
void sendDuty(float duty) {
  CAN_message_t m;
  m.id = VESC_ID | ((uint32_t)PKT_SET_DUTY << 8);
  m.flags.extended = 1;
  m.len = 4;
  int32_t v = (int32_t)lroundf(duty * 100000.0f);
  m.buf[0] = (v >> 24) & 0xFF;
  m.buf[1] = (v >> 16) & 0xFF;
  m.buf[2] = (v >> 8)  & 0xFF;
  m.buf[3] =  v        & 0xFF;
  can1.write(m);
}

// Kill switch: zero immediately (no ramp) and latch until 'g'.
void stopNow(const char *why) {
  stopped = true;
  targetDuty = 0.0f;
  cmdDuty = 0.0f;
  Serial.print("** STOPPED ("); Serial.print(why); Serial.println(") — type g to re-arm **");
}

// One typed line: "g" or "d<percent>". ('s' is handled per keystroke.)
void handleCommand(const char *line) {
  if (line[0] == '\0') return;
  if (line[0] == 'g' || line[0] == 'G') {
    stopped = false;
    targetDuty = 0.0f;
#if STAGE >= 3
    armed = false;
    Serial.println("** Re-armed. Release both sensors to start **");
#else
    Serial.println("** Re-armed. Duty is 0 until you type dN **");
#endif
    return;
  }
#if STAGE == 2
  if (line[0] == 'd' || line[0] == 'D') {
    if (stopped) { Serial.println("** Ignored: stopped. Type g first **"); return; }
    char *end;
    long pct = strtol(line + 1, &end, 10);
    if (end == line + 1 || *end != '\0') { Serial.println("** Bad duty. Use e.g. d5 or d-5 **"); return; }
    if (labs(pct) > (long)(MAX_DUTY * 100.0f + 0.5f)) {
      Serial.print("** Rejected: max is d"); Serial.print((int)(MAX_DUTY * 100.0f + 0.5f));
      Serial.println(" **");
      return;
    }
    targetDuty = pct / 100.0f;
    Serial.print("** Target duty "); Serial.print(targetDuty, 2); Serial.println(" **");
    return;
  }
  Serial.println("** Commands: dN (e.g. d5, d-5, d0), s = stop, g = re-arm **");
#else
  Serial.println("** Commands: s = stop, g = re-arm (sensors control duty in Stage 3) **");
#endif
}

// Read serial a character at a time. 's' acts instantly, without Enter.
void handleSerial() {
  static char buf[16];
  static uint8_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 's' || c == 'S') { stopNow("typed s"); n = 0; continue; }
    if (c == '\r') continue;
    if (c == '\n') { buf[n] = '\0'; n = 0; handleCommand(buf); continue; }
    if (n < sizeof(buf) - 1) buf[n++] = c;
  }
}
#endif

#if STAGE >= 3
// One sensor's press -> duty magnitude: 0 at THRESH, MAX_DUTY at FULL.
float pressToDuty(int s) {
  if (s <= THRESH) return 0.0f;
  float x = (float)(s - THRESH) / (FULL - THRESH);
  if (x > 1.0f) x = 1.0f;
  return MAX_DUTY * x;
}

// Sensor 1 alone -> forward, sensor 2 alone -> reverse, both/neither -> 0.
float sensorsToDuty(int s1, int s2) {
  bool p1 = s1 > THRESH, p2 = s2 > THRESH;
  if (p1 == p2) return 0.0f;
  return p1 ? pressToDuty(s1) : -pressToDuty(s2);
}
#endif

// ============================================================
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogReadAveraging(4);

  can1.begin();
  can1.setBaudRate(CAN_BAUD);
  can1.setMaxMB(16);
  can1.enableFIFO();
  can1.setFIFOFilter(ACCEPT_ALL);

  delay(200);
#if STAGE >= 3
  Serial.println("LEAN CAN motor test — STAGE 3: SENSORS IN CONTROL");
  Serial.println("  sensor 1 = forward, sensor 2 = reverse, both/neither = stop");
  Serial.println("  s = STOP + latch   g = re-arm   (release both sensors to start)");
#elif STAGE == 2
  Serial.println("LEAN CAN motor test — STAGE 2: TYPED DUTY");
  Serial.println("  dN = duty N% (d5, d-5, d0)   s = STOP + latch   g = re-arm");
#else
  Serial.println("LEAN CAN motor test — STAGE 1: LISTEN ONLY (no commands sent)");
#endif
#if STAGE >= 2
  Serial.print("  max duty "); Serial.print(MAX_DUTY, 2);
  Serial.println(", SET_DUTY sent at 50 Hz (zeros included)");
#endif
  Serial.print("VESC id "); Serial.print(VESC_ID);
  Serial.println(" at 500 kbps on CAN1 (TX 22 / RX 23)");
}

void loop() {
  // --- drain every received frame ---
  CAN_message_t msg;
  while (can1.read(msg)) handleFrame(msg);

#if STAGE >= 2
  handleSerial();

  // --- send SET_DUTY at 50 Hz, always, including zeros ---
  static uint32_t tSend = 0;
  if (millis() - tSend >= SEND_MS) {
    tSend = millis();
    bool statusLost = framesRx == 0 || millis() - lastStatusMs > STATUS_TIMEOUT_MS;

#if STAGE >= 3
    int raw1 = analogRead(PIN_S1);
    int raw2 = analogRead(PIN_S2);
    int s1 = aboveFloor(raw1), s2 = aboveFloor(raw2);
    bool released = s1 <= THRESH && s2 <= THRESH;

    bool fault = raw1 >= RAIL || raw2 >= RAIL;
    if (fault && !sensorFault) Serial.println("** SENSOR FAULT: reading near 3.3 V — duty 0 **");
    sensorFault = fault;

    // Any fault or lost CAN disarms; control resumes only after both sensors
    // are released, so the motor never starts from a press already held.
    if (fault || statusLost) armed = false;
    else if (!armed && released) armed = true;

    targetDuty = (armed && !stopped) ? sensorsToDuty(s1, s2) : 0.0f;
#else
    // Lost VESC status = CAN problem: drop to 0 and require a new command.
    if (statusLost && targetDuty != 0.0f) {
      targetDuty = 0.0f;
      Serial.println("** VESC status lost — duty set to 0 **");
    }
#endif
    if (stopped) { targetDuty = 0.0f; cmdDuty = 0.0f; }

    // slew limit: 0 -> MAX_DUTY in no less than RAMP_S
    float d = targetDuty - cmdDuty;
    if (d >  SLEW_PER_SEND) d =  SLEW_PER_SEND;
    if (d < -SLEW_PER_SEND) d = -SLEW_PER_SEND;
    cmdDuty += d;

    sendDuty(cmdDuty);
  }
#endif

  // --- print at 10 Hz ---
  static uint32_t tPrint = 0;
  if (millis() - tPrint < 100) return;
  tPrint = millis();

  int s1 = readSensor(PIN_S1);
  int s2 = readSensor(PIN_S2);

  // CAN error counters (ECR: TX in bits 0-7, RX in bits 8-15).
  // Rising counts = wiring/bitrate problem even if some frames get through.
  uint32_t ecr = FLEXCAN1_ECR;
  uint8_t txErr = ecr & 0xFF;
  uint8_t rxErr = (ecr >> 8) & 0xFF;

  Serial.printf("s1=%4d s2=%4d  ", s1, s2);
#if STAGE >= 3
  const char *state = stopped ? " STOPPED" : sensorFault ? " FAULT" : !armed ? " RELEASE" : "";
  Serial.printf("cmd=%+.3f%s | ", cmdDuty, state);
#elif STAGE == 2
  Serial.printf("cmd=%+.3f%s | ", cmdDuty, stopped ? " STOPPED" : "");
#else
  Serial.print("cmd=none | ");
#endif
  if (gotStatus)  Serial.printf("ERPM=%6ld  duty=%+.3f  I=%5.1fA  ", (long)erpm, vescDuty, motorCurrent);
  else            Serial.print("ERPM=    --  ");
  if (gotStatus4) Serial.printf("FET=%5.1fC  ", mosfetTempC); else Serial.print("FET=   --   ");
  if (gotStatus5) Serial.printf("Vin=%5.1fV  ", inputVoltage); else Serial.print("Vin=   --   ");
  Serial.printf("frames=%lu  canErr rx=%u tx=%u\n", (unsigned long)framesRx, rxErr, txErr);

  // --- diagnostics ---
  uint32_t silentMs = millis() - lastStatusMs;
  if (framesRx == 0 || silentMs > STATUS_TIMEOUT_MS) {
    Serial.print("  !! NO VESC STATUS ");
    if (framesRx == 0) Serial.print("(none yet)");
    else { Serial.print("for "); Serial.print(silentMs); Serial.print(" ms"); }
    Serial.println(" — check STBY->GND, VIO->3.3V, 60 ohm CANH-CANL, TX/RX, 500k, status msgs on");
  }
  if (otherId >= 0 && millis() - lastOtherMs < STATUS_TIMEOUT_MS) {   // only while still arriving
    Serial.printf("  !! status frames from controller id %d, expected %d — check Controller ID in VESC Tool\n",
                  otherId, VESC_ID);
  }
}
