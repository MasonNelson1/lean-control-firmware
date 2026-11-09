#include <Arduino.h>
#include <ezButton.h>
#include "vesc_can.h"
#include "rpm_setpoint_pid.h"
#include "common.h"


using namespace common;

// Calculate once; configurable knee coordinates live in drive_constants.h.
const float INPUT_RESPONSE_EXPONENT =
    logf(INPUT_RESPONSE_KNEE_OUTPUT) / logf(INPUT_RESPONSE_KNEE_INPUT);

/** Map a normalized [-1, 1] lean input through the configured power curve.
 *  Preserves direction, zero and full scale; call after the input deadband.
 */
float applyLeanResponseCurve(float input) {
  return copysignf(powf(fabsf(input), INPUT_RESPONSE_EXPONENT), input);
}




// Runtime knob selections: kept at the hard design limits until knobs are wired.
float userMaxSpeedMS = MAX_SPEED;
float userMaxAccelMS2 = MAX_ACCEL;
// Future integration (once per VESC update):
// leftRPMSetpointPID.smoothRPM(leftTargetRPM, dt, userMaxSpeedMS, userMaxAccelMS2);
// Likewise for right. A stop/fault must bypass smoothing and reset BOTH instances.

// MISC CONSTANTS and vars
ezButton calibrateButton(PIN_CAL_BUTTON);
ezButton leanModeButton(PIN_LEAN_BUTTON);
ezButton remoteModeButton(PIN_REMOTE_BUTTON);

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


float leftRpms = 0.0f, rightRpms = 0.0f;
bool  stopped = false;
int   unknownId = -1;            // a VESC on the bus that isn't LEFT or RIGHT
int   floorF = 0, floorB = 0, floorL = 0, floorR = 0;

int aboveFloor(int raw, int floor) { return raw > floor ? raw - floor : 0; }


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
  Serial1.printf("** Calibrated (%d samples): floor F=%d B=%d L=%d R=%d **\n",
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



static mode MODE = DEFAULT_MODE;
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogReadAveraging(4);
  calibrateButton.setDebounceTime(BUTTON_DEBOUNCE);
  leanModeButton.setDebounceTime(BUTTON_DEBOUNCE);
  remoteModeButton.setDebounceTime(BUTTON_DEBOUNCE);
  vescBegin(can1);
  delay(200);
  remote_setup(); 
  Serial.printf("LEAN SIMPLE DRIVE: left VESC %d, right VESC %d.  s = stop, g = go\n",
                VESC_LEFT_ID, VESC_RIGHT_ID);
  Serial.printf("  calibrate: button on pin %d, seat empty. Floors start at %d\n",
                PIN_CAL_BUTTON, DEFAULT_FLOOR);
  
}

void loop() {
  leanModeButton.loop();
  remoteModeButton.loop();
  calibrateButton.loop();

  if (leanModeButton.isPressed() && (MODE != LEAN)) {
    MODE = LEAN;
    Serial.println("Entering LEAN mode");
  }else if (remoteModeButton.isPressed() && (MODE != REMOTE)) {
    MODE = REMOTE;
    Serial.println("Entering remote mode. ");
  }
  int f = 0, b = 0, l = 0, r = 0, total = 0;
  float speed = 0.0f, turn = 0.0f;
  static float leftTargetRPM = 0.0f, rightTargetRPM = 0.0f;
  if (MODE == LEAN) {
    //lean mode
    //this debounces the button and calibrates if the user wants to calibrate. 
    // for some reason this has to be the first thing in loop() because of interference from other stuff like the can or something. 
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
      }
    }
    // --- read, minus each sensor's floor ---
    f = aboveFloor(analogRead(PIN_F), floorF);
    b = aboveFloor(analogRead(PIN_B), floorB);
    l = aboveFloor(analogRead(PIN_L), floorL);
    r = aboveFloor(analogRead(PIN_R), floorR);

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

    total = f + b + l + r;
    // Seat detection is currently disabled in the stop condition.

    //calculate speed and turn components from -1 to 1 
    speed = normalizeSensorPair(f - b, backSaturation, frontSaturation);
    turn = normalizeSensorPair(r - l, leftSaturation, rightSaturation);
    
    //apply deadband. 
    if (fabs(speed) < DEADBAND) speed = 0.0f;
    if (fabs(turn) < DEADBAND)  turn = 0.0f;


    

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
      //todo implement estop pid tuning
      leftRPMSetpointPID.reset();
      rightRPMSetpointPID.reset();
      leftRpms = 0;
      rightRpms = 0;
      setMotorPower(leftRpms, rightRpms);
    } else {
      //todo replace MAX_ACCEL with min(accel, MAX_ACCEL) where accel is the user set value from the control box. 
      //also do the same with the max speeds. 
      //also need seperate angular max speed to match specs maybe. 
      setMotorPower(speed + turn, speed - turn);
    }

  } else if(MODE == REMOTE) {
    //remote mode
    remote_loop();
  }else {
    stopped = true;//should never happen
    Serial.println("Unrecognized mode");
  }


 

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
