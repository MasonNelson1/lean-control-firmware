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

// Interrupts latch raw presses; ezButton confirms the debounced press in loop().
// Never call ezButton, print, or command motors from an interrupt handler.
static volatile bool leanEdgePending = false;
static volatile bool remoteEdgePending = false;

void onLeanButtonPress() {
  leanEdgePending = true;
}

void onRemoteButtonPress() {
  remoteEdgePending = true;
}

const char *modeName(mode currentMode) {
  switch (currentMode) {
    case LEAN: return "LEAN";
    case REMOTE: return "REMOTE";
    default: return "UNKNOWN";
  }
}

void printMode() {
  Serial.printf("Mode: %s\n", modeName(MODE));
}
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogReadAveraging(4);
  calibrateButton.setDebounceTime(BUTTON_DEBOUNCE);
  leanModeButton.setDebounceTime(BUTTON_DEBOUNCE);
  remoteModeButton.setDebounceTime(BUTTON_DEBOUNCE);
  // ezButton configures the pins as INPUT_PULLUP: pressing connects to GND.
  attachInterrupt(digitalPinToInterrupt(PIN_LEAN_BUTTON), onLeanButtonPress, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_REMOTE_BUTTON), onRemoteButtonPress, FALLING);
  beginMotorCan();
  delay(200);
  remote_setup();
  printMode();
  Serial.printf("LEAN SIMPLE DRIVE: left VESC %d, right VESC %d.  s = stop, g = go\n",
                VESC_LEFT_ID, VESC_RIGHT_ID);
  Serial.printf("  calibrate: button on pin %d, seat empty. Floors start at %d\n",
                PIN_CAL_BUTTON, DEFAULT_FLOOR);
  
}

void loop() {
  static bool leanPressPending = false;
  static bool remotePressPending = false;
  noInterrupts();
  const bool leanEdge = leanEdgePending;
  const bool remoteEdge = remoteEdgePending;
  leanEdgePending = false;
  remoteEdgePending = false;
  interrupts();
  leanPressPending |= leanEdge;
  remotePressPending |= remoteEdge;

  leanModeButton.loop();
  remoteModeButton.loop();
  calibrateButton.loop();

  const bool leanPressed = leanModeButton.isPressed() && leanPressPending;
  const bool remotePressed = remoteModeButton.isPressed() && remotePressPending;
  if (leanPressed) leanPressPending = false;
  if (remotePressed) remotePressPending = false;
  if (leanModeButton.isReleased()) leanPressPending = false;
  if (remoteModeButton.isReleased()) remotePressPending = false;

  if (leanPressed && (MODE != LEAN)) {
    MODE = LEAN;
    printMode();
  } else if (remotePressed && (MODE != REMOTE)) {
    MODE = REMOTE;
    printMode();
  }
  int f = 0, b = 0, l = 0, r = 0, total = 0;
  float speed = 0.0f, turn = 0.0f;
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
    while (readMotorCan(msg)) {
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
      setMotorPower(0.0, 0.0);
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
  }


 

  // remote_loop() rate-limits motor updates, not this outer loop. Keep USB
  // telemetry bounded in BOTH modes so ezButton can be polled frequently.
  static uint32_t lastTelemetryMs = 0;
  const uint32_t now = millis();
  if (now - lastTelemetryMs < 50) return;  // 20 Hz telemetry
  lastTelemetryMs = now;

  // Print CSV header once
  static bool headerPrinted = false;
  if (!headerPrinted && Serial && Serial.availableForWrite() >= 512) {
    Serial.println("time_ms,F,B,L,R,total,userWeight,speed,turn,leftTargetRPM,rightTargetRPM,leftSmoothedRPM,rightSmoothedRPM,stopped,mode");
    headerPrinted = true;
  }

  // Print CSV row
  const MotorCommandTelemetry &motorTelemetry = getMotorCommandTelemetry();
  char row[256];
  const int rowLength = snprintf(row, sizeof(row),
    "%lu,%d,%d,%d,%d,%d,%d,%0.4f,%0.4f,%.4f,%.4f,%.4f,%.4f,%d,%s\n",
    millis(),
    f, b, l, r, total,
    userWeightEstimate,
    speed, turn,
    motorTelemetry.leftTargetRPM, motorTelemetry.rightTargetRPM,
    motorTelemetry.leftSmoothedRPM, motorTelemetry.rightSmoothedRPM,
    stopped ? 1 : 0, modeName(MODE)
);
  Serial.write(reinterpret_cast<const uint8_t *>(row), rowLength);
  // HC-06 telemetry is read-only and only emitted in lean mode. It does not
  // depend on a USB connection or change the remote command protocol.
  if (MODE == LEAN && Serial1.availableForWrite() >= rowLength) {
    Serial1.write(reinterpret_cast<const uint8_t *>(row), rowLength);
  }

}
