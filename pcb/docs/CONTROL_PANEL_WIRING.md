# Control-panel wiring - revision M prototype

All panel connector bodies face the rear. Pad 1 is square. Read physical pad numbers and housing orientation, not cable colors or apparent left/right order in a front view. Every interboard cable below is **straight pin-to-pin**, even though the rear headers appear mirrored when viewed from the front.

| Main board | Panel rear | Wires | Function |
|---|---|---:|---|
| J4 | J11 | 5 | Speed rotary |
| J3 | J12 | 5 | Acceleration rotary |
| J5 | J13 | 2 | Calibration input |
| J10 | J14 | 10 | Panel power, mode inputs, LED controls |
| J23 | J17 | 2 | Power-switch signal and ground |

J3/J4/J11/J12 use JST-PH PHR-5 cable housings. J10/J14 use PHR-10; J5/J13 and J23/J17 use PHR-2. All use PH crimp contacts chosen for wire gauge. All five harnesses are needed. Actual cable length is unknown; choose routing away from motor/phase wires, provide strain relief, and verify noise immunity and voltage drop at the installed length. This design has direct wired analog/GPIO signals, not a serial or differential panel link.

## Rotary switches

Both A10403RNCQ switches solder directly onto the panel. SW101 is SPEED, SW102 ACCEL. Each switch has six physical terminals: common A, positions 1-4, and isolated support terminal 9. Terminal 9 has a solder pad but no electrical connection.

| J11/J12 pin | Rotary terminal | Main-board destination |
|---:|---|---|
| 1 | A / common | SPEED A0/GPIO14/U7 pad36 or ACCEL A1/GPIO15/pad37 |
| 2 | 1 | 25% resistor selection |
| 3 | 2 | 50% resistor selection |
| 4 | 3 | 75% resistor selection |
| 5 | 4 | 100% resistor selection |

The ten resistor-network parts remain on the main board. The common does connect to the Teensy's analog pin through J11-J4 or J12-J3. See `ROTARY_WIRING.md` for nominal voltages, tolerances and invalid-input detection; percentage meanings are firmware settings, not a property of the mechanical switch.

## Wired momentary buttons

The selected Dirvcp B0FM8QKSM1 buttons mount in the enclosure and connect with their existing two leads. At **each J18-J21, pin 1 is signal and pin 2 is ground**. Either button lead can be signal; the dry contacts have no polarity.

| Panel connector | Function | Teensy destination |
|---|---|---|
| J18 | Calibrate | GPIO4, via J13.2 -> main J5.2 |
| J19 | LEAN mode | GPIO24 / U7 pad16 |
| J20 | Disarm | GPIO25 / U7 pad17 |
| J21 | Remote mode | GPIO26 / U7 pad18 |

Inputs are active-low. Calibration needs the Teensy's firmware pull-up enabled. The three new mode inputs have 10 kohm external pull-ups to 3.3 V, 1 kohm series resistors and 100 nF capacitors on the main board. Firmware debouncing and fault handling remain required. Disarm is an ordinary controller input, not a motor-current switch.

J13.1 goes to ground and J13.2 to Cal signal. This is intentionally a different function ordering from local button header J18; the **interboard J13-J5 cable remains pin1-to-pin1, pin2-to-pin2**.

## Main I/O cable: J10 to J14

| Pin | Function | Teensy GPIO / footprint pad |
|---:|---|---|
| 1 | GND | Ground return |
| 2 | 5 V supply to panel | From primary VESC through main board |
| 3 | LEAN button raw input | GPIO24 / pad16, through filter |
| 4 | Disarm button raw input | GPIO25 / pad17, through filter |
| 5 | Remote button raw input | GPIO26 / pad18, through filter |
| 6 | Cal LED | GPIO27 / pad19 |
| 7 | LEAN LED | GPIO28 / pad20 |
| 8 | Disarm LED | GPIO29 / pad21 |
| 9 | Remote LED | GPIO30 / pad22 |
| 10 | Remote-connected LED | GPIO31 / pad23 |

Five individual main-board 1 kohm resistors R101-R105 connect GPIO27-31 to J10 pins6-10, respectively. The J10-J14 cable carries these current-limited LED signals. Each signal goes directly to its indicator anode; each cathode goes to CP_GND. HIGH turns the LED on. U101 has been removed. The main-board nets after these resistors are PANEL_LED_CAL/LEAN/DISARM/REMOTE/CONNECTED; the corresponding panel nets are CP_LED_*.

Use 5 mm low-current red LEDs, nominally WP7113LID or a verified lab-stock equivalent. At 3.3 V through 1 kohm, expected normal current is approximately 1-2 mA per LED, below PJRC's recommended 4 mA output current. A cable short to ground after the resistor is limited to approximately 3.3 mA; this does not protect against a short to another voltage or cable transients. Check all-five-on brightness, current and noise behavior with the installed cable. Mount LEDs at the height required by the enclosure.

The owned power switch connects at J16. Panel J17 runs to main J23 (1=signal, 2=ground). Main J24.8 and J25.8 distribute that signal to the two VESC COMM power-switch inputs. J24.3 is primary ground; all other positions, including J25.3, are NC. All VESC cables originate on the main board. Use separate PHR-8 cables; the old two-pin J24 harness no longer fits. See `POWER_SWITCH.md`. The power-button LED uses 5 V at J16.3; Teensy GPIO signals remain 3.3 V.

The PCB uses distinct main-board and CP_ panel net names so KiCad does not demand copper between two separate physical boards. These names join only through the cables documented here. Connect and continuity-check all cables with power off.

Revision K (2026-10-14) changes only the panel outline, mounting pattern and rear component placement/routing. All connector pin numbers, electrical nets, five interboard harnesses and rear mounting sides remain unchanged. See PANEL_MECHANICAL.md for the V3 casing fit.

Revision L (2026-10-19): main-board J1 is now four-pin XH, 1=NC, 2=CANH, 3=CANL, 4=NC. This does not alter any panel harness or J24 power-switch wiring.

Revision M (2026-10-19): main J24/J25 now match the eight-position VESC COMM order. No panel geometry, copper or cable pinout changed.
