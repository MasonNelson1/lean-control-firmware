# lean-control-firmware

See [RPM setpoint PID documentation](docs/RPM_SETPOINT_PID.md) for the controller API, equations, tuning, limits and current firmware integration.

## Mode and telemetry components

`src/DriveController` is everything between the inputs and the one motor
command sent each control tick: the mode state machine, fault checks (failed
seat sensor, silent VESC), the drive modes, motion smoothing and stopping.
`main.cpp` only handles hardware: buttons, knobs, CAN, LEDs, serial and the
watchdog. The controller has no hardware dependency beyond `analogRead`, so
`tests/native/drive_controller_test.cpp` runs whole rides on the host.

`src/ModeStateMachine` owns selected/current modes, pending button presses,
Assist hold timing, profile confirmation, calibration requests, and the seat
latch. Selected mode survives seat loss; effective mode updates at the control
tick before motor commands.

### Calibration button

- **In LEAN, short press**: capture neutral. Let go and sit still; the LED
  blinks slowly for about a second and goes out when the chair is ready. The
  button is ignored while the chair moves faster than a slow walk; a chair
  that is only creeping is stopped first.
- **In LEAN, hold for one second** (LED starts blinking fast): range
  calibration. Let go, sit still, then lean as far as is comfortable forward,
  back, left and right, and press the button again. The four full-lean ranges
  are saved. A direction leaned less than 120 counts keeps its old range. The
  chair ends in DISARM.
- **In DISARM with the seat empty, hold for two seconds**: tare. Each sensor's
  zero is measured and saved. Do this once, and again after the cushion or
  sensors are moved. It is refused while the seat reads as occupied.

### Other panel behaviour

- The chair always powers on in DISARM. If it is rolling at power-on (a reset
  while driving), it brakes to a stop first.
- **DISARM** always stops the chair, from every mode, however long it is held.
- **Assist** needs its own press: with the chair already disarmed for at least
  a second, hold DISARM for half a second. It adds motor current for a moment
  after each push on the rims (the wheel must speed up by itself first) and
  gives nothing otherwise, so it cannot start or keep the chair moving. An
  empty seat ends it; it has to be selected again.
- A chair that nothing is driving is held: a wheel that starts to roll gets
  brake current. This is damping, not a parking brake (on a slope the chair
  creeps). It is not held in Assist, or in DISARM with a rider in the seat:
  there the rider has the rims. After a fault stop it is held even with a
  rider aboard, until DISARM is pressed.
- **To push the empty chair by hand**, tap DISARM while it is disarmed with
  the seat empty. The DISARM LED winks off once a second while it free-wheels.
  Tap again, sit down or select a mode to end it.
- **Reverse** is half the forward speed limit. A back lean that starts while
  the chair rolls forward only brakes it to a stop; come back to neutral
  before leaning back to reverse.
- If the log says "No valid saved settings were found" at power-on, the
  settings bank was lost (for example power cut during a save): tare and
  calibrate again, and check the dashboard settings.
- A fast-blinking DISARM LED means a fault disarmed the chair (failed seat
  sensor, silent VESC, watchdog restart). The dashboard's health badge names
  it. It clears when a mode is armed again.
- **REMOTE** ignores the stick until it has been centred once after REMOTE is
  selected, after a soft stop, and after the controller has been out of
  contact for half a second.

`src/telemetry/TelemetryTypes.h` defines all shared telemetry structs, including
Lean and Remote samples. Drive modes produce these samples. `TelemetryFrame`
formats the existing 19-column CSV and expires motor RPM feedback after 500 ms.
`Telemetry` owns the 20 Hz cadence, serial output, and `logf()` status messages.
USB receives a header and CSV rows, replacing the temporary voltage-only debug
output. Bluetooth retains headerless CSV and drops whole lines if its transmit
buffer is full. Binary configuration responses keep their PacketSerial framing
and run before periodic telemetry.

Run all host-side tests (native C++ with `g++`, plus the dashboard's JS and
Python tests) from this folder:

```sh
./run-tests.sh           # everything
./run-tests.sh native    # firmware only
```

Coverage includes button priorities, debounce latches, seat recovery, hold and
release boundaries, interrupted profile confirmations, clock rollover, CSV
columns, feedback expiry, serial backpressure, lean math and calibration,
stopping, and simulated rides with faults. Hardware behavior still requires
verification on the chair.




TODO:

outstanding possible issues from astra. I don't want to play with the code any more but need to test it on the wheelchair before messing with stuff. 

1. High — A lost configuration acknowledgment can make “Revert” falsely report success.
   Consider a clean configuration: the device accepts a Set request, but its reply is lost. The browser times out with the old field value and link.dirty === false. Clicking Revert then skips the device Revert command, clears the draft, and displays “Reverted to saved settings.” The device still has the changed value in RAM. The summary can also incorrectly say its values match saved settings.
   Recommendation: treat timed-out mutations as having an unknown outcome; reconcile with the device before claiming success or allowing a local-only revert.
   References: config-protocol.js:266 (config-protocol.js:266), configuration.js:691 (configuration.js:691).
3. Medium — “Exact-value” inputs conceal the values actually being applied.
   Floating-point inputs display only two decimal places while retaining a different precise value in dataset.precise. For example, 0.035 displays as 0.04, and a valid response-curve coordinate of 0.001 displays as 0. Applying the form uses the hidden precise value. Distinct configurations therefore look identical, and retyping the displayed number can change the setting.
   how to fis: display 3 digits instead of 2. 
   Reference: configuration.js:267 (configuration.js:267).
4. Medium — Valid multi-field changes can be impossible to apply directly.
   The browser sends each draft as a separate Set, and firmware validates the VESC-ID inequality after every field. Swapping the default IDs from (26, 2) to (2, 26) fails whichever field is sent first, although the final pair is valid. Other earlier edits may already have taken effect when this failure occurs.
   Recommendation: support an atomic update for related fields, or explicitly guide the user through a temporary unused ID.
   References: configuration.js:206 (configuration.js:206), DriveSettings.cpp:135 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/config/DriveSettings.cpp:135).
7.  Medium — A missing Bluetooth delimiter can prevent framing recovery.
    BluetoothReceiver toggles between text and binary mode on every zero byte. Losing one delimiter shifts that alternation: subsequent CSV can be consumed as binary, while valid replies are consumed as text. The frame-length limit bounds memory but does not reset the framing state. Reconnecting resets it.
    Recommendation: add a recovery strategy for invalid/oversized frames or incomplete-frame timeouts.
    Reference: bluetooth.js:28 (bluetooth.js:28).
