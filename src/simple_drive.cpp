#include <Arduino.h>
#include "vesc_can.h"
//test to see if caleb can commit. 
// ============================================================
// LEAN — SIMPLE DRIVE: 4 raw sensors -> two VESCs. Nothing else.
// Build + upload:  pio run -e simple -t upload
//
//   each sensor value = raw - that sensor's floor   (counts, >= 0)
//   speed = front - back
//   turn  = right - left
//   left wheel  lean = speed + turn
//   right wheel lean = speed - turn
//   each wheel: duty = 0 inside DEADBAND, then ramps up to MAX_DUTY
//
// Lean forward -> both forward. Lean right -> left wheel faster (turns right).
//
// Kept on purpose, a few lines each:
//   - empty seat -> 0
//   - slew limit (duty changes smoothly)
//   - duty sent every 10 ms, zeros included (if the Teensy dies, the
//     VESCs stop on their own timeout)
//   - type  s  in the monitor = stop (latched), then  g  = go
//
// Calibrate: press the button with the seat EMPTY and the wheels stopped.
// Each sensor's average over CAL_SAMPLES readings becomes its own floor.
// Until then every sensor uses FLOOR.
// ============================================================

// ---------- PINS / VESCs ----------
const uint8_t PIN_F = A0;    // pin 14
const uint8_t PIN_B = A17;   // pin 41
const uint8_t PIN_L = A14;   // pin 38
const uint8_t PIN_R = A16;   // pin 40
const uint8_t PIN_CAL_BUTTON = 4;    // button to GND (INPUT_PULLUP)
const int     CAL_SAMPLES    = 20;   // readings averaged per sensor (~1 ms each)

const uint8_t VESC_LEFT_ID  = 26;
const uint8_t VESC_RIGHT_ID = 2;     // set to the second VESC's Controller ID (VESC Tool)
const float   DIR_LEFT  = +1.0f;     // -1 if leaning forward spins that wheel backward
const float   DIR_RIGHT = +1.0f;     // mirrored motor: expect this one to need -1

// ---------- TUNING (all in raw ADC counts) ----------
const int   DEFAULT_FLOOR      = 400;    // default floor for every sensor until calibrated
// const int   SEATED_MIN = 150;    // total above floor, all 4 sensors
const int   DEADBAND   = 50;    // |lean| ignored below this (sitting still)
const int   FULL       = 1200;   // |lean| that gives MAX_DUTY
const float MAX_DUTY   = 0.60f;
const float GAIN = 2;
const float REAR_GAIN = 4.0f;
const float FRONT_GAIN = 1.0f;
const float LEFT_GAIN = 1.0f;
const float RIGHT_GAIN = 1.0f;


const float RAMP_S     = 1.0f;   // 0 -> MAX_DUTY takes at least this long
const uint32_t SEND_MS = 10;     // 100 Hz

VescBus can1;
VescStatus vescs[2] = { {VESC_LEFT_ID}, {VESC_RIGHT_ID} };
float dutyL = 0.0f, dutyR = 0.0f;
bool  stopped = false;
int   unknownId = -1;            // a VESC on the bus that isn't LEFT or RIGHT
int   floorF = DEFAULT_FLOOR, floorB = DEFAULT_FLOOR, floorL = DEFAULT_FLOOR, floorR = DEFAULT_FLOOR;

int aboveFloor(int raw, int floor) { return raw > floor ? raw - floor : 0; }

// Average CAL_SAMPLES readings of each sensor; each average becomes that
// sensor's floor. Only while both wheels are stopped. Blocks ~CAL_SAMPLES ms
// (no CAN commands meanwhile; harmless since the wheels are stopped).
void calibrate() {
  //only calibrate when wheels are stopped
  // if (fabs(dutyL) > 0.01f || fabs(dutyR) > 0.01f) {
  //   Serial.println("** Calibrate ignored: wheels must be stopped **");
  //   return;
  // }
  long sumF = 0, sumB = 0, sumL = 0, sumR = 0;
  for (int i = 0; i < CAL_SAMPLES; i++) {
    sumF += analogRead(PIN_F);
    sumB += analogRead(PIN_B);
    sumL += analogRead(PIN_L);
    sumR += analogRead(PIN_R);
    delay(2);
  }
  floorF = (sumF + CAL_SAMPLES / 2) / CAL_SAMPLES;
  floorB = (sumB + CAL_SAMPLES / 2) / CAL_SAMPLES;
  floorL = (sumL + CAL_SAMPLES / 2) / CAL_SAMPLES;
  floorR = (sumR + CAL_SAMPLES / 2) / CAL_SAMPLES;
  Serial.printf("** Calibrated (%d samples): floor F=%d B=%d L=%d R=%d **\n",
                CAL_SAMPLES, floorF, floorB, floorL, floorR);
}

// One wheel's lean (counts) -> target duty.
float leanToDuty(int lean) {
  if (abs(lean) <= DEADBAND) return 0.0f;
  float x = (float)(abs(lean) - DEADBAND) / (FULL - DEADBAND) * GAIN;
  if (x > 1.0f) x = 1.0f;
  return (lean > 0 ? 1.0f : -1.0f) * x * MAX_DUTY;
}

// Move cur toward target, no faster than RAMP_S for 0 -> MAX_DUTY.
float ramp(float cur, float target) {
  float step = MAX_DUTY * (SEND_MS / 1000.0f) / RAMP_S;
  return cur + constrain(target - cur, -step, step);
}

void printVesc(const char *name, const VescStatus &v) {
  if (v.frames == 0) Serial.printf(" | %s: no status", name);
  else Serial.printf(" | %s: %6ld erpm %5.1fA %3.0fC", name, (long)v.erpm, v.current, v.mosfetTempC);
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogReadAveraging(4);
  pinMode(PIN_CAL_BUTTON, INPUT_PULLUP);
  vescBegin(can1);
  delay(200);
  Serial.printf("LEAN SIMPLE DRIVE: left VESC %d, right VESC %d.  s = stop, g = go\n",
                VESC_LEFT_ID, VESC_RIGHT_ID);
  Serial.printf("  calibrate: button on pin %d, seat empty. Floors start at %d\n",
                PIN_CAL_BUTTON, DEFAULT_FLOOR);
}

void loop() {
  // --- receive VESC status (for the printout only) ---
  CAN_message_t msg;
  while (can1.read(msg)) {
    vescHandleFrame(msg, vescs, 2);
    uint8_t id = msg.id & 0xFF, type = (msg.id >> 8) & 0xFF;
    if (msg.flags.extended && type == VESC_STATUS && id != VESC_LEFT_ID && id != VESC_RIGHT_ID)
      unknownId = id;
  }

  // --- s = stop, g = go ---
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 's' || c == 'S') { stopped = true;  Serial.println("** STOPPED (g to go) **"); }
    if (c == 'g' || c == 'G') { stopped = false; Serial.println("** GO **"); }
  }

  // --- calibrate button (to GND): act on the press, 50 ms debounce ---
  static bool lastBtn = HIGH;
  static uint32_t tBtn = 0;
  bool btn = digitalRead(PIN_CAL_BUTTON);
  if (btn != lastBtn && millis() - tBtn > 50) {
    tBtn = millis();
    lastBtn = btn;
    if (btn == LOW) calibrate();
  }

  static uint32_t tSend = 0;
  if (millis() - tSend < SEND_MS) return;
  tSend = millis();

  // --- read, minus each sensor's floor ---
  int f = aboveFloor(analogRead(PIN_F), floorF);
  int b = aboveFloor(analogRead(PIN_B), floorB);
  int l = aboveFloor(analogRead(PIN_L), floorL);
  int r = aboveFloor(analogRead(PIN_R), floorR);

  //apply sensor specific gains
  b = (int)(b * REAR_GAIN);
  f = (int)(f * FRONT_GAIN);
  l = (int)(l * LEFT_GAIN);
  r = (int)(r * RIGHT_GAIN);


  int total = f + b + l + r;
  // bool seated = total >= SEATED_MIN;
  int speed = f - b;
  int turn  = r - l;

  // --- stop now, or ramp each wheel toward its target ---
  if (stopped /*|| !seated*/) {
    dutyL = 0.0f;
    dutyR = 0.0f;
  } else {
    dutyL = ramp(dutyL, leanToDuty(speed + turn));
    dutyR = ramp(dutyR, leanToDuty(speed - turn));
  }

  vescSetDuty(can1, VESC_LEFT_ID,  dutyL * DIR_LEFT);
  vescSetDuty(can1, VESC_RIGHT_ID, dutyR * DIR_RIGHT);

  // --- print 5x a second ---
  static int n = 0;
  if (n++ % 20 != 0) return;
  if (n % 400 == 1)
    Serial.println("\n  (sensor values = raw - floor)\n     F     B     L     R  total seat  speed   turn   dutyL   dutyR | VESCs");
  Serial.printf("  %4d  %4d  %4d  %4d  %5d   %s  %+5d  %+5d  %+.3f  %+.3f", f, b, l, r, total,
                true ? "Y" : "N", speed, turn, dutyL, dutyR);
  printVesc("L", vescs[0]);
  printVesc("R", vescs[1]);
  if (unknownId >= 0) Serial.printf(" | other VESC id %d on bus", unknownId);
  Serial.println(stopped ? "  STOPPED" : "");
}
