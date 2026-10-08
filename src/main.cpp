#include <Arduino.h>

// Forward declaration: captureNeutral() is called from readRaw() (sim "cal"
// command) before it is defined. The Arduino IDE generated this
// automatically for .ino files; a .cpp file needs it written out.
void captureNeutral();

// ============================================================
// LEAN Wheelchair — Four-Sensor Lean Control (core firmware)
// ------------------------------------------------------------
// Reads four force sensors in a PLUS formation, computes a
// center-of-pressure lean signal on two axes, and mixes them
// into left/right motor commands.
//
//        FRONT  = forward          Plus formation:
//        BACK   = backward                 [F]
//        LEFT   = turn left            [L] [ ] [R]
//        RIGHT  = turn right               [B]
//
// Two axes, each from an opposing pair:
//    speed  from FRONT / BACK
//    turn   from RIGHT / LEFT
// Mixed into differential drive:  left = speed+turn, right = speed-turn
//
// SIM_MODE lets you test ALL of this logic now, with no hardware,
// by typing sensor values into the Serial Monitor.
//
// Board: Teensy 4.1 | USB Type: Serial | Serial Monitor baud: 115200
// ============================================================

// ---------- SIM MODE ----------
//   1 = type "F,B,L,R" (e.g. 800,400,500,500) into Serial to test the math
//       type "cal" to capture the neutral posture
//   0 = read the real sensor pins
#define SIM_MODE 0

// ---------- RAW MONITOR (sensor bring-up) ----------
//   1 = only print raw ADC counts from MONITOR_PINS at 10 Hz. The control
//       pipeline is skipped entirely, so motor outputs stay at 0.
//   0 = normal control loop
#define RAW_MONITOR 0
const uint8_t MONITOR_PINS[]  = {A14, A17, A16, A15};   // F,B,L,R = pins 38,41,40,39
const char*   MONITOR_NAMES[] = {"F", "B", "L", "R"};
const int     N_MONITOR = sizeof(MONITOR_PINS) / sizeof(MONITOR_PINS[0]);

// ---------- PIN MAP (set to match your wiring) ----------
const uint8_t PIN_FRONT = A14;   // pin 38
const uint8_t PIN_BACK  = A17;   // pin 41
const uint8_t PIN_LEFT  = A16;   // pin 40
const uint8_t PIN_RIGHT = A15;   // pin 39
const uint8_t PIN_CAL   = 4;        // calibration ("zero") button to GND
const uint8_t PIN_SPEED_POT = A6;   // pin 20 — max speed knob wiper
const uint8_t PIN_ACCEL_POT = A7;   // pin 21 — acceleration knob wiper

// 1 = read the real knobs (works even in SIM_MODE — test real knobs
//     against simulated sensors). 0 = use the default values below.
// Keep 0 until the switch ladders are wired: unwired A6/A7 float and
// would read random positions.
#define USE_POTS 0

// ---------- TUNABLES (measure/tune on real hardware) ----------
const float   FLOOR         = 370.0f;  // no-load ADC reading per channel (the 0.3V bias)
const float   OCCUPANCY_MIN = 150.0f;  // floor-subtracted total to count as "seated"
const float   DEADZONE      = 0.08f;   // ignore lean smaller than this (fraction of range)
// Speed and acceleration settings come from the rotary switch tables
// (SPEED_TABLE / ACCEL_TABLE, next to updateKnobs()).
// Defaults used when USE_POTS = 0
const float   DEFAULT_SPEED = 0.50f;
const float   DEFAULT_ACCEL = 1.5f;
const float   CAL_STILL_MAX = 0.05f;   // only allow "zero" when the chair is essentially stopped
const uint32_t LOOP_US      = 1000;    // 1 kHz control loop
const float   EPS           = 1.0f;    // guards divide-by-zero in normalization

// ---------- STATE ----------
float neutralSpeed = 0.0f;   // CoP of the front/back axis at neutral posture
float neutralTurn  = 0.0f;   // CoP of the right/left axis at neutral posture
float outLeft = 0.0f, outRight = 0.0f;   // slew-limited motor commands (-1..+1)
bool  lastCal = HIGH;
uint32_t calChangedMs = 0;              // for button debounce
bool  calStable = HIGH;

float maxOutput   = DEFAULT_SPEED;                      // set by speed knob
float accelTimeS  = DEFAULT_ACCEL;                      // set by accel knob
float slewPerLoop = (LOOP_US / 1e6f) / DEFAULT_ACCEL;   // derived from accelTimeS

// ---------- SMALL HELPERS ----------

// Normalized center of pressure along one axis -> -1..+1.
// Divides by the TOTAL force on all four sensors, not just this pair:
// dividing by the pair alone turned a few counts of noise on an unloaded
// pair into a full ±1 lean. With the total, |speed| + |turn| <= 1, so the
// differential mix can never exceed ±1.
float coP(float pos, float neg, float total) {
  if (total < EPS) return 0.0f;           // no force at all
  return (pos - neg) / (total + EPS);
}

// Deadzone with rescale: 0 inside the band, ramps smoothly 0..1 outside
// (no sudden jump at the edge).
float deadzone(float x) {
  if (x >  DEADZONE) return (x - DEADZONE) / (1.0f - DEADZONE);
  if (x < -DEADZONE) return (x + DEADZONE) / (1.0f - DEADZONE);
  return 0.0f;
}

float clampUnit(float x) { return x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x); }

// Move 'cur' toward 'target' by at most slewPerLoop per call.
// slewPerLoop is set by the acceleration knob.
float slew(float cur, float target) {
  float d = target - cur;
  if (d >  slewPerLoop) d =  slewPerLoop;
  if (d < -slewPerLoop) d = -slewPerLoop;
  return cur + d;
}

// Last raw readings, kept for the live printout.
float rawF = 0, rawB = 0, rawL = 0, rawR = 0;

// Real-sensor mode: typing "cal" captures neutral (same as the zero
// button), so the math can be tested before the button is wired.
void readSerialCommands() {
  static String buf = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      buf.trim();
      if (buf.equalsIgnoreCase("cal")) {
        bool stopped = fabs(outLeft) < CAL_STILL_MAX && fabs(outRight) < CAL_STILL_MAX;
        if (stopped) captureNeutral();
        else Serial.println("** Zero ignored: chair must be stopped **");
      }
      buf = "";
    } else if (c != '\r') {
      buf += c;
    }
  }
}

// ---------- RAW SENSOR INPUT (real or simulated) ----------
void readRaw(float &f, float &b, float &l, float &r) {
#if SIM_MODE
  static float sf = FLOOR, sb = FLOOR, sl = FLOOR, sr = FLOOR;
  static String buf = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      buf.trim();
      if (buf.equalsIgnoreCase("cal")) {
        captureNeutral();                 // typing "cal" calibrates in sim
      } else {
        int v[4], n = 0, start = 0;
        for (int i = 0; i <= (int)buf.length() && n < 4; i++) {
          if (i == (int)buf.length() || buf[i] == ',') {
            v[n++] = buf.substring(start, i).toInt();
            start = i + 1;
          }
        }
        if (n == 4) { sf = v[0]; sb = v[1]; sl = v[2]; sr = v[3]; }
      }
      buf = "";
    } else if (c != '\r') {
      buf += c;
    }
  }
  f = sf; b = sb; l = sl; r = sr;
#else
  f = analogRead(PIN_FRONT);
  b = analogRead(PIN_BACK);
  l = analogRead(PIN_LEFT);
  r = analogRead(PIN_RIGHT);
#endif
  rawF = f; rawB = b; rawL = l; rawR = r;
}

// Floor-subtracted forces (0 = no load). This is what the CoP uses.
void readForces(float &f, float &b, float &l, float &r) {
  float rf, rb, rl, rr;
  readRaw(rf, rb, rl, rr);
  f = rf - FLOOR; if (f < 0) f = 0;
  b = rb - FLOOR; if (b < 0) b = 0;
  l = rl - FLOOR; if (l < 0) l = 0;
  r = rr - FLOOR; if (r < 0) r = 0;
}

// ---------- KNOBS (rotary selector switches on a resistor ladder) ----------
// Each switch has N_POS detent positions. A resistor ladder puts each
// position at its own voltage, so we snap the reading to the nearest
// position and only accept it once it has been stable for ~60 ms
// (3 consecutive 20 ms reads). Position 1 sits at GND, so a broken or
// unplugged wire reads as position 1 = the gentlest setting (fail-safe).
const int   N_POS = 5;
const float SPEED_TABLE[N_POS] = {0.20f, 0.40f, 0.60f, 0.80f, 1.00f}; // max output
const float ACCEL_TABLE[N_POS] = {3.0f,  2.0f,  1.5f,  1.0f,  0.5f }; // seconds 0->full

int snapToPosition(int raw) {                  // 0..4095 -> 0..N_POS-1
  int p = (int)((raw / 4095.0f) * (N_POS - 1) + 0.5f);
  if (p < 0) p = 0;
  if (p > N_POS - 1) p = N_POS - 1;
  return p;
}

// Accept a new position only after it reads the same 3 times in a row.
// Filters the brief float/short while the switch is between detents.
int stablePosition(int raw, int &accepted, int &candidate, int &count) {
  int p = snapToPosition(raw);
  if (p == candidate) { if (count < 3) count++; }
  else { candidate = p; count = 1; }
  if (count >= 3) accepted = candidate;
  return accepted;
}

int speedPos = 0, accelPos = 0;                // current accepted positions (0-based)

void updateKnobs() {
#if USE_POTS
  static int spAcc = 0, spCand = -1, spCnt = 0;
  static int acAcc = 0, acCand = -1, acCnt = 0;
  speedPos = stablePosition(analogRead(PIN_SPEED_POT), spAcc, spCand, spCnt);
  accelPos = stablePosition(analogRead(PIN_ACCEL_POT), acAcc, acCand, acCnt);
  maxOutput  = SPEED_TABLE[speedPos];
  accelTimeS = ACCEL_TABLE[accelPos];
#else
  maxOutput  = DEFAULT_SPEED;
  accelTimeS = DEFAULT_ACCEL;
#endif
  slewPerLoop = (LOOP_US / 1e6f) / accelTimeS;  // full-scale step per loop
}

// Plain-language direction the chair would go, from the post-deadzone
// speed (+fwd) and turn (+right). Display only.
const char* directionName(bool seated, float speed, float turn) {
  if (!seated) return "STOP (empty seat)";
  if (speed == 0 && turn == 0) return "STOP (neutral)";
  if (speed == 0) return turn > 0 ? "PIVOT RIGHT" : "PIVOT LEFT";
  if (turn == 0)  return speed > 0 ? "FORWARD" : "BACKWARD";
  if (speed > 0)  return turn > 0 ? "FORWARD + RIGHT" : "FORWARD + LEFT";
  return turn > 0 ? "BACKWARD + RIGHT" : "BACKWARD + LEFT";
}

// ---------- CALIBRATION ----------
// Capture the rider's neutral posture as the "zero lean" center of pressure.
void captureNeutral() {
  float f, b, l, r;
  readForces(f, b, l, r);
  float total = f + b + l + r;
  if (total < OCCUPANCY_MIN) {            // empty seat: CoP would be pure noise
    Serial.println("** Zero ignored: nobody seated **");
    return;
  }
  neutralSpeed = coP(f, b, total);
  neutralTurn  = coP(r, l, total);
  Serial.println("** Neutral posture captured **");
}

// ============================================================
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);     // needed for real sensors AND real knobs
  analogReadAveraging(4);
  pinMode(PIN_CAL, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  delay(200);
  Serial.println("LEAN control firmware ready.");
#if RAW_MONITOR
  Serial.println("RAW MONITOR: raw ADC counts (0-4095) and volts, control disabled");
#elif SIM_MODE
  Serial.println("SIM MODE:");
  Serial.println("  type  F,B,L,R   e.g. 800,400,500,500   (raw ADC values)");
  Serial.println("  type  cal       to capture neutral posture");
#else
  Serial.println("SENSOR MODE: reading A14-A17 (F,B,L,R). Motor output is print-only.");
  Serial.println("  type  cal  (or press zero button) to capture neutral posture");
#endif
}

void loop() {
  // --- fixed-rate control loop (rollover-safe) ---
  static uint32_t last = 0;
  if ((uint32_t)(micros() - last) < LOOP_US) return;
  last += LOOP_US;

#if RAW_MONITOR
  // --- bring-up: print raw sensor readings, no control output ---
  static uint32_t tMon = 0;
  if (millis() - tMon >= 100) {          // 10 Hz, readable
    tMon = millis();
    for (int i = 0; i < N_MONITOR; i++) {
      int raw = analogRead(MONITOR_PINS[i]);
      Serial.print(MONITOR_NAMES[i]); Serial.print("=");
      Serial.print(raw);
      Serial.print(" ("); Serial.print(raw * 3.3f / 4095.0f, 2); Serial.print("V)   ");
    }
    Serial.println();
  }
  return;
#endif

  // --- knobs: update every 20 ms ---
  static uint32_t tKnob = 0;
  if (millis() - tKnob >= 20) { tKnob = millis(); updateKnobs(); }

#if !SIM_MODE
  readSerialCommands();
#endif

  // --- calibration ("zero") button: debounced, and only while stopped ---
  bool raw = digitalRead(PIN_CAL);
  if (raw != lastCal) { calChangedMs = millis(); lastCal = raw; }
  if (millis() - calChangedMs > 50 && raw != calStable) {   // stable 50 ms
    calStable = raw;
    if (calStable == LOW) {                                  // a real press
      bool stopped = fabs(outLeft) < CAL_STILL_MAX && fabs(outRight) < CAL_STILL_MAX;
      if (stopped) {
        captureNeutral();
        digitalWrite(LED_BUILTIN, HIGH);                     // brief confirm blink
      } else {
        Serial.println("** Zero ignored: chair must be stopped **");
      }
    } else {
      digitalWrite(LED_BUILTIN, LOW);
    }
  }

  // --- read forces ---
  float f, b, l, r;
  readForces(f, b, l, r);
  float total = f + b + l + r;

  float targetLeft, targetRight;
  float speed = 0, turn = 0;            // kept outside for printing
  float copSpeed = 0, copTurn = 0;      // raw CoP before neutral/deadzone (printing)

  // --- occupancy gate: empty seat OR disconnected sensor -> safe stop ---
  if (total < OCCUPANCY_MIN) {
    targetLeft = 0; targetRight = 0;
  } else {
    // --- center of pressure on each axis, relative to neutral posture ---
    copSpeed = coP(f, b, total);
    copTurn  = coP(r, l, total);
    speed = clampUnit(copSpeed - neutralSpeed);    // forward(+)/back(-)
    turn  = clampUnit(copTurn  - neutralTurn);     // right(+)/left(-)

    // --- deadzone ---
    speed = deadzone(speed);
    turn  = deadzone(turn);

    // --- mix to differential drive ---
    float left  = speed + turn;
    float right = speed - turn;

    // --- if mixing pushed past ±1, scale both down (keeps the turn ratio) ---
    float m = max(fabs(left), fabs(right));
    if (m > 1.0f) { left /= m; right /= m; }

    targetLeft  = left  * maxOutput;     // speed knob caps the output
    targetRight = right * maxOutput;
  }

  // --- slew-rate limit (smooth, no instant jumps) ---
  outLeft  = slew(outLeft,  targetLeft);
  outRight = slew(outRight, targetRight);

  // --- OUTPUT ---
  // TODO (later): send outLeft / outRight as current commands over CAN to the
  // two VESCs. For now, print so you can watch the behavior.
  static uint32_t tPrint = 0;
  if (millis() - tPrint >= 100) {        // 10 Hz, readable
    tPrint = millis();
    bool seated = total >= OCCUPANCY_MIN;
    Serial.print("raw F="); Serial.print(rawF, 0);
    Serial.print(" B="); Serial.print(rawB, 0);
    Serial.print(" L="); Serial.print(rawL, 0);
    Serial.print(" R="); Serial.print(rawR, 0);
    Serial.print("  tot="); Serial.print(total, 0);
    Serial.print(seated ? " Y" : " N");
    Serial.print(" | CoP fb="); Serial.print(copSpeed, 2);
    Serial.print(" rl="); Serial.print(copTurn, 2);
    Serial.print(" | spd="); Serial.print(speed, 2);
    Serial.print(" trn="); Serial.print(turn, 2);
    Serial.print(" | L="); Serial.print(outLeft, 2);
    Serial.print(" R="); Serial.print(outRight, 2);
    Serial.print(" | "); Serial.println(directionName(seated, speed, turn));
  }
}
