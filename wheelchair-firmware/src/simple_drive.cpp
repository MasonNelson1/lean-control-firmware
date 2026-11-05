#include <Arduino.h>
#include <ezButton.h>
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
const uint8_t PIN_CAL_BUTTON = 4;    
const int     CAL_SAMPLES    = 20;   // readings averaged per sensor (~1 ms each)
const float WHEEL_CIRCUMFERENCE_M = 74.75f * 0.0254f; //meters
const float MAX_ACCEL = 2.85f;// m/s^2
const float MAX_SPEED = 4.91744f;//11mph but in m/s

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

// MISC CONSTANTS and vars
ezButton calibrateButton(PIN_CAL_BUTTON);
const int CALIBRATE_BUTTON_DEBOUNCE = 50;//ms

//calibration constants
int userWeightEstimate = 370;//default to unloaded sensor values. 

//these constants decide how much a sensor needs to be triggered in relation to 
//the weight estimate to be considered full input. 
//so a value of one means if the sensor is reading a value equal to the user's weight, the the max value is reached. 
//the smaller the value, the more responsive the chair is. 
int frontWeightFactor = 1;
int backWeightFactor = 1;
int leftWeightFactor = 1;
int rightWeightFactor = 1;

//these values are set in calibration and are initialized unreasonably high to preclude 
//accidental wheel movement. 
int frontSaturation = 1000000;
int backSaturation = 1000000;
int leftSaturation = 1000000;
int rightSaturation = 1000000;


boolean initiallyCalibrated = false;

const float RAMP_S     = 0.5f;   // 0 -> MAX_DUTY takes at least this long
const uint32_t LOOP_RATE = 5;     // 200 Hz

VescBus can1;
VescStatus vescs[2] = { {VESC_LEFT_ID}, {VESC_RIGHT_ID} };
float leftRpms = 0.0f, rightRpms = 0.0f;
bool  stopped = false;
int   unknownId = -1;            // a VESC on the bus that isn't LEFT or RIGHT
int   floorF = 0, floorB = 0, floorL = 0, floorR = 0;

int aboveFloor(int raw, int floor) { return raw > floor ? raw - floor : 0; }




// Convert linear acceleration (m/s^2) to angular acceleration (RPM/s)
float linearAccelToRPMs(float accel) {
    return accel * (60.0f / WHEEL_CIRCUMFERENCE_M);
}

// Convert angular acceleration (RPM/s) to linear acceleration (m/s^2)
float rpmsToLinearAccel(float rpms) {
    return rpms * (WHEEL_CIRCUMFERENCE_M / 60.0f);
}

float speedToRPM(float speed) {
  return linearAccelToRPMs(speed);
}

// Returns the next RPM, towards a target at the provided linear acceleration in m/s^2. 
float accelToTarget(float currentRPM, float desiredRPM, float accelRate) {
    const float dt = LOOP_RATE / 1000.0f;

    // Maximum RPM change this loop.
    const float maxStep = linearAccelToRPMs(accelRate) * dt;

    return currentRPM + constrain(desiredRPM - currentRPM, -maxStep, maxStep);
}

// Average CAL_SAMPLES readings of each sensor; each average becomes that
// sensor's floor. Only while both wheels are stopped. Blocks ~CAL_SAMPLES ms
// (no CAN commands meanwhile; harmless since the wheels are stopped).
void calibrate() {
  
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
  userWeightEstimate = (floorF + floorB + floorL + floorR) / 4;//integer division ok here.

  frontSaturation = frontWeightFactor * userWeightEstimate;
  backSaturation = backWeightFactor * userWeightEstimate;
  leftSaturation = leftWeightFactor * userWeightEstimate;
  rightSaturation = rightWeightFactor * userWeightEstimate;
  

  if (!initiallyCalibrated) {
    initiallyCalibrated = true;
    stopped = false;
  }

  Serial.printf("** Calibrated (%d samples): floor F=%d B=%d L=%d R=%d **\n",
                CAL_SAMPLES, floorF, floorB, floorL, floorR);
}

float normalizeSensorPair(int difference, int negativeLimit, int positiveLimit) {
    if (difference >= 0) {
        return constrain(
            (float)difference / positiveLimit,
            0.0f,
            1.0f
        );
    } else {
        return constrain(
            (float)difference / negativeLimit,
            -1.0f,
            0.0f
        );
    }
}



void printVesc(const char *name, const VescStatus &v) {
  if (v.frames == 0) Serial.printf(" | %s: no status", name);
  else Serial.printf(" | %s: %6ld erpm %5.1fA %3.0fC", name, (long)v.erpm, v.current, v.mosfetTempC);
}


void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogReadAveraging(4);
  calibrateButton.setDebounceTime(CALIBRATE_BUTTON_DEBOUNCE);
  vescBegin(can1);
  delay(200);
  Serial.printf("LEAN SIMPLE DRIVE: left VESC %d, right VESC %d.  s = stop, g = go\n",
                VESC_LEFT_ID, VESC_RIGHT_ID);
  Serial.printf("  calibrate: button on pin %d, seat empty. Floors start at %d\n",
                PIN_CAL_BUTTON, DEFAULT_FLOOR);
  
}

void loop() {
  //this debounces the button and calibrates if the user wants to calibrate. 
  // for some reason this has to be the first thing in loop() because of interference from other stuff like the can or something. 
  calibrateButton.loop();
  if (calibrateButton.isPressed()) {
    calibrate();
  }

  //force calibration before the wheels can move.
  if (!initiallyCalibrated) {
    stopped = true;
  }


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

  //apply saturations
  f = min(frontSaturation, f);
  b = min(frontSaturation, b);
  l = min(frontSaturation, l);
  r = min(frontSaturation, r);

  

  int total = f + b + l + r;
  bool seated = total >= userWeightEstimate * .75f;

  //calculate speed and turn components from -1 to 1
  float speed = normalizeSensorPair(f - b, backSaturation, frontSaturation);
  float turn  = normalizeSensorPair(r - l, leftSaturation, rightSaturation);


  // --- stop now, or ramp each wheel toward its target ---
  if (stopped /*|| !seated*/) {
    leftRpms = 0.0f;
    rightRpms = 0.0f;
  } else {
    //todo replace MAX_ACCEL with min(accel, MAX_ACCEL) where accel is the user set value from the control box. 
    //also do the same with the max speeds. 
    //also need seperate angular max speed to match specs maybe. 
    float leftTargetRPM = constrain(speed + turn, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);
    float rightTargetRPM = constrain(speed - turn, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);
    leftRpms = accelToTarget(leftRpms, leftTargetRPM, MAX_ACCEL);
    rightRpms = accelToTarget(rightRpms, rightTargetRPM, MAX_ACCEL);
    // leftRpms = accelToTarget(leftRpms,  map(speed + turn, -1, 1, -speedToRPM(MAX_SPEED), speedToRPM(MAX_SPEED)), MAX_ACCEL);
    // rightRpms = accelToTarget(rightRpms,  map(speed - turn, -1, 1, -speedToRPM(MAX_SPEED), speedToRPM(MAX_SPEED)), MAX_ACCEL);
  }


  //rate limit the loop. 
  //this is placed right before the motor communication to try and make that the most consistently limited thing. 
  static uint32_t tSend = 0;
  if (millis() - tSend < LOOP_RATE) {
    return;
  }
  tSend = millis();

  // vescSetDuty(can1, VESC_LEFT_ID,  dutyL * DIR_LEFT);
  // vescSetDuty(can1, VESC_RIGHT_ID, dutyR * DIR_RIGHT);
     vescSetWheelRPM(can1, VESC_LEFT_ID, leftRpms);
    vescSetWheelRPM(can1, VESC_RIGHT_ID, rightRpms);
 

  // // --- print 5x a second ---
  static int n = 0;
  if (n++ % 20 != 0) return;
  if (n % 400 == 1)
    Serial.println("\n  (sensor values = raw - floor)\n     F     B     L     R  total usrWeight  speed   turn   dutyL   dutyR | VESCs");
  Serial.printf("  %4d  %4d  %4d  %4d  %5d   %4d  %+.3f  %+.3f  %+.3f  %+.3f", f, b, l, r, total,
               userWeightEstimate, speed, turn, leftRpms, rightRpms);
  printVesc("L", vescs[0]);
  printVesc("R", vescs[1]);
  if (unknownId >= 0) Serial.printf(" | other VESC id %d on bus", unknownId);
  Serial.println(stopped ? "  STOPPED" : "");
  
  
  //csv debug printout
//   static int n = 0;
//   if (n++ % 5 != 0) return;

//   // Print CSV header once
//   if (n == 1) {
//     Serial.println("time_ms,F,B,L,R,total,userWeight,speed,turn,dutyL,dutyR,stopped");
//   }

//   // Print CSV row
//   Serial.printf(
//     "%lu,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%d\n",
//     millis(),
//     f, b, l, r, total,
//     userWeightEstimate,
//     speed, turn,
//     dutyL, dutyR,
//     stopped ? 1 : 0
// );
  
}
