#include <Arduino.h>
#include <ezButton.h>
#include "vesc_can.h"
#include "rpm_setpoint_pid.h"
#include "drive_constants.h"

using namespace DriveConstants;

// Calculate once; configurable knee coordinates live in drive_constants.h.
const float INPUT_RESPONSE_EXPONENT =
    logf(INPUT_RESPONSE_KNEE_OUTPUT) / logf(INPUT_RESPONSE_KNEE_INPUT);

/** Map a normalized [-1, 1] lean input through the configured power curve.
 *  Preserves direction, zero and full scale; call after the input deadband.
 */
float applyLeanResponseCurve(float input) {
  return copysignf(powf(fabsf(input), INPUT_RESPONSE_EXPONENT), input);
}


// ---------- RPM SETPOINT PID  ----------
const RPMSetpointPID::Tuning RPM_PID_TUNING = {
    RPM_PID_LOW_SPEED_TAU_S, RPM_PID_HIGH_SPEED_TAU_S,
    RPM_PID_KI, RPM_PID_KD, RPM_PID_DERIVATIVE_FILTER_S,
    RPM_PID_LOW_SPEED_ACCEL_MS2, RPM_PID_HIGH_SPEED_ACCEL_MS2,
    RPM_PID_MAX_JERK_RPM_PER_S2};

RPMSetpointPID leftRPMSetpointPID(RPM_PID_TUNING);
RPMSetpointPID rightRPMSetpointPID(RPM_PID_TUNING);


// Runtime knob selections: kept at the hard design limits until knobs are wired.
float userMaxSpeedMS = MAX_SPEED;
float userMaxAccelMS2 = MAX_ACCEL;
// Future integration (once per VESC update):
// leftRPMSetpointPID.smoothRPM(leftTargetRPM, dt, userMaxSpeedMS, userMaxAccelMS2);
// Likewise for right. A stop/fault must bypass smoothing and reset BOTH instances.

// MISC CONSTANTS and vars
ezButton calibrateButton(PIN_CAL_BUTTON);

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


VescBus can1;
VescStatus vescs[2] = { {VESC_LEFT_ID}, {VESC_RIGHT_ID} };
float leftRpms = 0.0f, rightRpms = 0.0f;
bool  stopped = false;
int   unknownId = -1;            // a VESC on the bus that isn't LEFT or RIGHT
int   floorF = 0, floorB = 0, floorL = 0, floorR = 0;

int aboveFloor(int raw, int floor) { return raw > floor ? raw - floor : 0; }

// ---------- BLUETOOTH PRINTS ----------
auto &bluetooth = Serial1;                  // HC-06: pin 0 = RX1, pin 1 = TX1
const uint32_t BLUETOOTH_BAUD = 115200;     // must match the HC-06's baud setting
uint8_t bluetoothTxBuffer[512];             // extra room so prints don't slow down loop()





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
  bluetooth.printf("** Calibrated (%d samples): floor F=%d B=%d L=%d R=%d **\n",
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



void setup() {
  Serial.begin(115200);
  bluetooth.begin(BLUETOOTH_BAUD);
  bluetooth.addMemoryForWrite(bluetoothTxBuffer, sizeof(bluetoothTxBuffer));
  analogReadResolution(12);
  analogReadAveraging(4);
  calibrateButton.setDebounceTime(CALIBRATE_BUTTON_DEBOUNCE);
  vescBegin(can1);
  delay(200);
  Serial.printf("LEAN SIMPLE DRIVE: left VESC %d, right VESC %d.  s = stop, g = go\n",
                VESC_LEFT_ID, VESC_RIGHT_ID);
  bluetooth.printf("LEAN SIMPLE DRIVE: left VESC %d, right VESC %d.  s = stop, g = go\n",
                VESC_LEFT_ID, VESC_RIGHT_ID);
  Serial.printf("  calibrate: button on pin %d, seat empty. Floors start at %d\n",
                PIN_CAL_BUTTON, DEFAULT_FLOOR);
  bluetooth.printf("  calibrate: button on pin %d, seat empty. Floors start at %d\n",
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


  // --- receive VESC status (not used for control) ---
  CAN_message_t msg;
  while (can1.read(msg)) {
    vescHandleFrame(msg, vescs, 2);
    uint8_t id = msg.id & 0xFF, type = (msg.id >> 8) & 0xFF;
    if (msg.flags.extended && type == VESC_STATUS && id != VESC_LEFT_ID && id != VESC_RIGHT_ID && id != unknownId) {
      unknownId = id;
      Serial.printf("** other VESC id %d on bus **\n", unknownId);
      bluetooth.printf("** other VESC id %d on bus **\n", unknownId);
    }
  }

  // --- s = stop, g = go ---
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 's' || c == 'S') { stopped = true;  Serial.println("** STOPPED (g to go) **"); bluetooth.println("** STOPPED (g to go) **"); }
    if (c == 'g' || c == 'G') { stopped = false; Serial.println("** GO **"); bluetooth.println("** GO **"); }
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
  
  //apply deadband. 
  if (fabs(speed) < DEADBAND) speed = 0.0f;
  if (fabs(turn) < DEADBAND)  turn = 0.0f;

  static float leftTargetRPM = 0.0f;
  static float rightTargetRPM = 0.0f;

  //rate limit the loop. 
  //this is placed right before the motor communication and pid smoothing because pid needs to run at a fixed rate. 
  static uint32_t tSend = 0;
  if (millis() - tSend < LOOP_RATE) {
    return;
  }
  tSend = millis();

  // Shape speed and turn separately before wheel mixing and PID smoothing.
  speed = applyLeanResponseCurve(speed);
  turn = applyLeanResponseCurve(turn);

  // --- stop now, or ramp each wheel toward its target ---
  if (stopped /*|| !seated*/) {
    leftRpms = 0.0f;
    rightRpms = 0.0f;
    leftRPMSetpointPID.reset();
    rightRPMSetpointPID.reset();
  } else {
    //todo replace MAX_ACCEL with min(accel, MAX_ACCEL) where accel is the user set value from the control box. 
    //also do the same with the max speeds. 
    //also need seperate angular max speed to match specs maybe. 
    leftTargetRPM = constrain(speed + turn, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);
    rightTargetRPM = constrain(speed - turn, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);
    leftRpms = leftRPMSetpointPID.smoothRPM(leftTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
    rightRpms = rightRPMSetpointPID.smoothRPM(rightTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
  }

    vescSetWheelRPM(can1, VESC_LEFT_ID, leftRpms);
    vescSetWheelRPM(can1, VESC_RIGHT_ID, rightRpms);
 

  //csv telemetry printout, USB only: at this rate it would overrun the HC-06's 115200 baud
  static int n = 0;
  if (n++ % 1 != 0) return;

  // Print CSV header once
  if (n == 1) {
    Serial.println("time_ms,F,B,L,R,total,userWeight,speed,turn,leftTargetRPM,rightTargetRPM, leftSmoothedRPM, rightSmoothedRPM, ,stopped");
  }

  // Print CSV row
  Serial.printf(
    "%lu,%d,%d,%d,%d,%d,%d,%0.4f,%0.4f,%.4f,%.4f,%.4f,%.4f,%d\n",
    millis(),
    f, b, l, r, total,
    userWeightEstimate,
    speed, turn,
    leftTargetRPM, rightTargetRPM,leftRpms, rightRpms,
    stopped ? 1 : 0
);

}
