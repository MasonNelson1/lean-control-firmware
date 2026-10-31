#include <Arduino.h>
#include "vesc_can.h"

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
const uint8_t MONITOR_PINS[]  = {A15, A17, A14, A16};   // F,B,L,R = pins 39,41,38,40
const char*   MONITOR_NAMES[] = {"F", "B", "L", "R"};
const int     N_MONITOR = sizeof(MONITOR_PINS) / sizeof(MONITOR_PINS[0]);

// ---------- PIN MAP (set to match your wiring) ----------
const uint8_t PIN_FRONT = A15;   // pin 39
const uint8_t PIN_BACK  = A17;   // pin 41
const uint8_t PIN_LEFT  = A14;   // pin 38
const uint8_t PIN_RIGHT = A16;   // pin 40
const uint8_t PIN_CAL   = 4;        // calibration ("zero") button to GND
const uint8_t PIN_SPEED_POT = A6;   // pin 20 — max speed knob wiper
const uint8_t PIN_ACCEL_POT = A7;   // pin 21 — acceleration knob wiper

// 1 = read the real knobs (works even in SIM_MODE — test real knobs
//     against simulated sensors). 0 = use the default values below.
// Keep 0 until the switch ladders are wired: unwired A6/A7 float and
// would read random positions.
#define USE_POTS 0

// ---------- MOTOR OUTPUT (VESCs over CAN) ----------
//   1 = send outLeft / outRight to the VESCs.  0 = print only, no CAN at all.
#define CAN_OUTPUT 1

// Which sides have a VESC installed. Only these are commanded, and every
// one of them must be reporting status or all output is forced to 0.
#define DRIVE_LEFT  1      // VESC_LEFT_ID
#define DRIVE_RIGHT 0      // VESC_RIGHT_ID (not installed yet)
const uint8_t VESC_LEFT_ID  = 26;    // VESC Tool default for this unit
const uint8_t VESC_RIGHT_ID = 2;

// Flip a side (-1) if leaning forward spins that wheel backward.
// The motors are mirrored, so expect one side to need it.
const float LEFT_DIR  = +1.0f;
const float RIGHT_DIR = +1.0f;

//   1 = DUTY: output * MAX_DUTY. Speed follows the lean. Use for
//       wheels-off-the-ground tests (an unloaded wheel on current
//       control runs away to full speed).
//   0 = CURRENT: output * MAX_CURRENT_A. Torque control, for the floor.
#define OUTPUT_MODE_DUTY 1
const float    MAX_DUTY      = 0.30f;   // duty at full lean, speed switch at 100%
const float    MAX_CURRENT_A = 10.0f;   // amps at full lean, speed switch at 100%
const uint32_t CAN_SEND_MS   = 10;      // 100 Hz, zeros included
const uint32_t VESC_STATUS_TIMEOUT_MS = 200;   // silent this long = CAN fault

const int SENSOR_RAIL = 4000;   // raw reading this high = shorted sensor / wire on 3.3 V

#if SIM_MODE && CAN_OUTPUT
#error "SIM_MODE with CAN_OUTPUT would drive real motors from typed numbers. Set one to 0."
#endif
#if CAN_OUTPUT && !DRIVE_LEFT && !DRIVE_RIGHT
#error "CAN_OUTPUT is on but no side is enabled (DRIVE_LEFT / DRIVE_RIGHT)."
#endif

// ---------- TUNABLES (measure/tune on real hardware) ----------
const float   FLOOR         = 370.0f;  // no-load ADC reading per channel (the 0.3V bias)
const float   OCCUPANCY_MIN = 0.0f;  // floor-subtracted total to count as "seated"
const float   DEADZONE      = 0.08f;   // ignore lean smaller than this (fraction of range)
// A seated torso lean only moves the CoP a fraction of the way to ±1 (that
// would be all weight on one sensor). The gain scales the lean AFTER the
// deadzone, so the deadzone still ignores the same posture wobble.
// Full output at a post-deadzone lean of 1/gain.
const float   SPEED_GAIN    = 3.0f;    // forward/back
const float   TURN_GAIN     = 3.0f;    // right/left
// Speed and acceleration settings come from the rotary switch tables
// (SPEED_TABLE / ACCEL_TABLE, next to updateKnobs()).
// Defaults used when USE_POTS = 0
const float   DEFAULT_SPEED = 1.00f;
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

// Motor output is allowed unless: kill, a sensor at the 3.3 V rail, a VESC
// not reporting, or an empty seat. Any of those zeros output immediately.
bool  driving    = false;    // output allowed this loop (for the LED / printout)
bool  killed     = false;    // 's' typed: latched until 'g'

#if CAN_OUTPUT
VescBus can1;
VescStatus vescs[2] = { {VESC_LEFT_ID}, {VESC_RIGHT_ID} };
#endif

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
  if (total < EPS) return 0.0f;           // divide-by-zero guard: no load = neutral
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

// Real-sensor mode serial commands:
//   s   -> KILL: output to 0 now and latch (acts on the keypress, no Enter)
//   g   -> clear the kill (still needs neutral lean to re-arm)
//   cal -> capture neutral (same as the zero button), only while stopped
void readSerialCommands() {
  static String buf = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 's' || c == 'S') {
      killed = true;
      buf = "";
      Serial.println("** KILLED — output 0. Type g to clear **");
      continue;
    }
    if (c == '\n') {
      buf.trim();
      if (buf.equalsIgnoreCase("cal")) {
        bool stopped = fabs(outLeft) < CAL_STILL_MAX && fabs(outRight) < CAL_STILL_MAX;
        if (stopped) captureNeutral();
        else Serial.println("** Zero ignored: chair must be stopped **");
      } else if (buf.equalsIgnoreCase("g")) {
        killed = false;
        Serial.println("** Kill cleared **");
      }
      buf = "";
    } else if (c != '\r') {
      buf += c;
    }
  }
}

// ---------- RAW SENSOR INPUT (real or simulated) ----------
void readRaw(float &f, float &b, float &l, float &r) {
f = analogRead(PIN_FRONT);
  b = analogRead(PIN_BACK);
  l = analogRead(PIN_LEFT);
  r = analogRead(PIN_RIGHT);
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

// Built-in LED shows the state (no laptop needed):
//   off = empty seat   solid = driving   fast blink = fault or kill
void updateStatusLed(bool fault, bool seated) {
  uint32_t t = millis();
  bool on;
  if (fault)            on = (t / 100) % 2;    // 5 Hz
  else if (!seated)     on = false;
  else                  on = true;
  digitalWrite(LED_BUILTIN, on);
}

#if CAN_OUTPUT
// Every enabled VESC has reported status recently.
bool vescsReporting() {
  if (DRIVE_LEFT  && !vescFresh(vescs[0], VESC_STATUS_TIMEOUT_MS)) return false;
  if (DRIVE_RIGHT && !vescFresh(vescs[1], VESC_STATUS_TIMEOUT_MS)) return false;
  return true;
}

// One side's -1..+1 output -> a VESC command.
void sendMotor(uint8_t id, float out) {
#if OUTPUT_MODE_DUTY
  vescSetDuty(can1, id, out * MAX_DUTY);
#else
  vescSetCurrent(can1, id, out * MAX_CURRENT_A);
#endif
}

uint32_t busFrames = 0;      // every CAN frame received, from any id
int      busLastId = -1;     // controller id of the last extended frame

void printVesc(const char *name, const VescStatus &v) {
  if (v.frames == 0) {
    // Diagnosis: frames from another id = wrong Controller ID in VESC Tool.
    // No frames + tx errors climbing = nobody ACKs us (VESC off, no 5 V on
    // MCP2562 VDD, STBY high, CANH/CANL open). rx errors = baud/wiring/termination.
    uint32_t ecr = FLEXCAN1_ECR;
    Serial.printf(" %s: no status (bus frames=%lu", name, (unsigned long)busFrames);
    if (busLastId >= 0) Serial.printf(" last id=%d", busLastId);
    Serial.printf(" err rx=%lu tx=%lu)", (unsigned long)((ecr >> 8) & 0xFF), (unsigned long)(ecr & 0xFF));
    return;
  }
  Serial.printf(" %s: %6ld erpm %5.1fA %4.1fV %3.0fC", name, (long)v.erpm,
                v.current, v.inputVoltage, v.mosfetTempC);
}
#endif

// ============================================================
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);     // needed for real sensors AND real knobs
  analogReadAveraging(4);
  pinMode(PIN_CAL, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
#if CAN_OUTPUT
  vescBegin(can1);
#endif
  delay(200);
  Serial.println("LEAN control firmware ready.");
#if RAW_MONITOR
  Serial.println("RAW MONITOR: raw ADC counts (0-4095) and volts, control disabled");
#elif SIM_MODE
  Serial.println("SIM MODE:");
  Serial.println("  type  F,B,L,R   e.g. 800,400,500,500   (raw ADC values)");
  Serial.println("  type  cal       to capture neutral posture");
#else
  Serial.println("SENSOR MODE: reading A14-A17 (F,B,L,R)");
#if CAN_OUTPUT
  Serial.print("  MOTORS LIVE over CAN: ");
  if (DRIVE_LEFT)  Serial.printf("left=VESC %d  ", VESC_LEFT_ID);
  if (DRIVE_RIGHT) Serial.printf("right=VESC %d  ", VESC_RIGHT_ID);
#if OUTPUT_MODE_DUTY
  Serial.print("DUTY mode, max "); Serial.println(MAX_DUTY, 2);
#else
  Serial.print("CURRENT mode, max "); Serial.print(MAX_CURRENT_A, 1); Serial.println(" A");
#endif
#else
  Serial.println("  Motor output is print-only (CAN_OUTPUT 0)");
#endif
  Serial.println("  sit and lean to drive (optional: type  cal  to zero your posture)");
  Serial.println("  s = KILL (instant, latched)   g = clear kill");
#endif
}

void loop() {
#if CAN_OUTPUT
  // --- drain received VESC status frames ---
  CAN_message_t msg;
  while (can1.read(msg)) {
    busFrames++;                          // any frame at all, for diagnosis
    if (msg.flags.extended) busLastId = msg.id & 0xFF;
    vescHandleFrame(msg, vescs, 2);
  }
#endif

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
  /*bool raw = digitalRead(PIN_CAL);
  if (raw != lastCal) { calChangedMs = millis(); lastCal = raw; }
  if (millis() - calChangedMs > 50 && raw != calStable) {   // stable 50 ms
    calStable = raw;
    if (calStable == LOW) {                                  // a real press
      bool stopped = fabs(outLeft) < CAL_STILL_MAX && fabs(outRight) < CAL_STILL_MAX;
      if (stopped) captureNeutral();
      else Serial.println("** Zero ignored: chair must be stopped **");
    }
  }*/

  // --- read forces ---
  float f, b, l, r;
  readForces(f, b, l, r);
  f = 2100 - f;
  b = 2100 - b;
  l = 2100 - l;
  r = 2100 - r;

  float total = f + b + l + r;

  float targetLeft, targetRight;
  float speed = 0, turn = 0;            // kept outside for printing
  float copSpeed = 0, copTurn = 0;      // raw CoP before neutral/deadzone (printing)

  // --- occupancy gate: empty seat OR disconnected sensor -> safe stop ---
  if (total <= OCCUPANCY_MIN) {
    targetLeft = 0; targetRight = 0;
  } else {
    // --- center of pressure on each axis, relative to neutral posture ---
    // copSpeed = coP(f, b, total);
    // copTurn  = coP(r, l, total);
    // float forwardCommand = f - b;
    // float turnCommand = r - l;
    speed = clampUnit((f - b) / (f + b));    // forward(+)/back(-)
    turn  = clampUnit((r  - l) / (r + l));     // right(+)/left(-)

    // --- deadzone ---
    // speed = deadzone(speed);
    // turn  = deadzone(turn);

    // --- lean gain (after the deadzone) ---
    speed = clampUnit(speed * SPEED_GAIN);
    turn  = clampUnit(turn  * TURN_GAIN);

    // --- mix to differential drive ---
    float left  = speed + turn;
    float right = speed - turn;

    // --- if mixing pushed past ±1, scale both down (keeps the turn ratio) ---
    float m = max(fabs(left), fabs(right));
    if (m > 1.0f) { left /= m; right /= m; }

    targetLeft  = left  * maxOutput;     // speed knob caps the output
    targetRight = right * maxOutput;
  }

  // --- arming / faults ---
  bool seated = total >= OCCUPANCY_MIN;
  bool sensorFault = rawF >= SENSOR_RAIL || rawB >= SENSOR_RAIL ||
                     rawL >= SENSOR_RAIL || rawR >= SENSOR_RAIL;
#if CAN_OUTPUT
  bool canOk = vescsReporting();
#else
  bool canOk = true;
#endif
  driving = !(killed || sensorFault || !canOk || !seated);

  if (!driving) {
    // Fault, kill, or empty seat: zero immediately, no slew.
    outLeft = 0; outRight = 0;
  } else {
    // --- slew-rate limit (smooth, no instant jumps) ---
    outLeft  = slew(outLeft,  targetLeft);
    outRight = slew(outRight, targetRight);
  }
  updateStatusLed(killed || sensorFault || !canOk, seated);

  // --- OUTPUT: VESC commands at 100 Hz, every cycle, zeros included ---
#if CAN_OUTPUT
  static uint32_t tCan = 0;
  if (millis() - tCan >= CAN_SEND_MS) {
    tCan = millis();
    if (DRIVE_LEFT)  sendMotor(VESC_LEFT_ID,  outLeft  * LEFT_DIR);
    if (DRIVE_RIGHT) sendMotor(VESC_RIGHT_ID, outRight * RIGHT_DIR);
  }
#endif

  // --- printout: aligned columns, header every 20 lines, 5 Hz ---
  static uint32_t tPrint = 0;
  static int printLines = 0;
  if (millis() - tPrint >= 200) {
    tPrint = millis();

    // Why the motors are (not) allowed to move, first reason wins.
    const char *state = killed       ? "KILLED (g)"
                      : sensorFault  ? "SENSOR FAULT"
                      : !canOk       ? "NO VESC"
                      : !seated      ? "EMPTY SEAT"
                      :                "DRIVING";

    if (printLines++ % 20 == 0) {
      Serial.println();
      Serial.println("     F     B     L     R   total seat |  CoP fb    rl |   spd   trn |  outL  outR | state          | direction        | VESC");
    }
    Serial.printf("  %4.0f  %4.0f  %4.0f  %4.0f   %5.0f   %s  |  %+5.2f %+5.2f | %+5.2f %+5.2f | %+5.2f %+5.2f | %-14s | %-16s |",
                  f, b, l, r, total, seated ? "Y" : "N",
                  copSpeed, copTurn, speed, turn, outLeft, outRight,
                  state, directionName(seated, speed, turn));
#if CAN_OUTPUT
    if (DRIVE_LEFT)  printVesc("L", vescs[0]);
    if (DRIVE_RIGHT) printVesc("R", vescs[1]);
#endif
    Serial.println();
  }
}
