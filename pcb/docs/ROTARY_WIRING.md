# Wiring the two A10403RNCQ rotary switches

> Revision G update (2026-10-11): the two A10403RNCQ switches now solder onto the control panel. Connect main J4 to rear panel J11 and main J3 to rear panel J12 with straight pin-to-pin cables. The panel copper performs the switch-terminal wiring below. The resistor networks and A0/A1 mapping remain unchanged. See CONTROL_PANEL_WIRING.md for the full panel.


Use one switch for maximum speed (J4) and one for acceleration (J3). Each selects **25%, 50%, 75% or 100%**, with the percentages assigned in firmware. Both switches solder onto the remote control panel and connect by cable to five-pin JST-PH headers on the main board. ACCEL is left and SPEED is right when viewed from the panel front, following the teammate drawing.

**The analog connections are present:** J4 pin 1 goes to **A0 / GPIO 14** (footprint U7 pad 36), and J3 pin 1 goes to **A1 / GPIO 15** (U7 pad 37). These are two different numbering systems: footprint pad 36 is not Teensy GPIO 36. R49/C2 and R54/C1 branch off those same analog wires; the signal does not stop at the resistors.

See `rotary-wiring.png` for the five-wire terminal map and `analog-paths.png` for the actual copper routes highlighted by net. In the schematic, matching global labels such as SPEED_ADC represent an electrical connection even when no long wire is drawn between them.

## Why the switch has six pins

The A10403RNCQ is a **single-pole, four-position** switch. Its electrical terminals are common **A** and contacts **1, 2, 3, 4**. Because this part uses the **C / PCB through-hole** terminal option, it also has terminal **9**, which is only mechanical support and has no internal electrical connection. Leave terminal 9 unwired.

Read the numbers molded on the **bottom of the housing**. Do not count the six physical pins sequentially or infer pin numbers from their positions in a photograph. The manufacturer's bottom-view diagrams and support-pin note are on pages 3 and 5 of `references/Littelfuse-CK-Rotary-A-Series-Datasheet.pdf`.

The code specifies a 0.375-inch flatted shaft, 3/8-32 threaded bushing, non-shorting contacts, PCB terminals and silver contacts. The four-position stops are factory-set. The datasheet identifies position 1 as fully counterclockwise at the shaft. Confirm terminal continuity before attaching the knob's scale.

## Cable connections — identical for both controls

| PCB connector pin | Switch terminal marking | Function |
|---:|---|---|
| 1 | A | Common, read by Teensy ADC |
| 2 | 1 | Position 1: 25% |
| 3 | 2 | Position 2: 50% |
| 4 | 3 | Position 3: 75% |
| 5 | 4 | Position 4: 100% |
| No wire | 9 | Mechanical support only |

J4 common is **A0 / GPIO 14**; J3 common is **A1 / GPIO 15**. The board header is **B5B-PH-K-S**, 2.00 mm pitch; its cable housing is **PHR-5**. Use five matching PH crimp contacts per cable. The connector does not supply an external potentiometer's 3.3 V / wiper / ground arrangement; all five pins have the new functions above.

All ten resistors are on the main PCB. The panel copper connects the rotary PCB terminals to J11/J12, so no resistor ladder or direct wire joints need to be added at the switch. Provide strain relief for the connector harnesses. Terminal 9 is soldered for mechanical support and remains unused electrically.

## On-board circuit and expected readings

Each common has a 4.7 kΩ pull-up to 3.3 V. The four selected contacts connect it to GND through 1 kΩ, 2.2 kΩ, 4.7 kΩ or 10 kΩ respectively. All ten resistors must be **1% tolerance**, nominal 0.25 W axial parts with 7.62 mm lead spacing. Existing C2 (SPEED) and C1 (ACCEL), each 100 nF, filter the ADC inputs.

| Function | SPEED | ACCEL | Value |
|---|---|---|---|
| Pull-up | R49 | R54 | 4.7 kΩ |
| Position 1 branch | R50 | R55 | 1 kΩ |
| Position 2 branch | R51 | R56 | 2.2 kΩ |
| Position 3 branch | R52 | R57 | 4.7 kΩ |
| Position 4 branch | R53 | R58 | 10 kΩ |

The nominal ratio is `selected_resistance / (4700 + selected_resistance)`. Voltage values assume a measured 3.300 V supply; ADC counts assume 10-bit conversion with the 3.3 V rail as reference.

| Position / setting | Nominal voltage | Nominal ADC / 1023 | Suggested accepted ratio | Approximate 10-bit accepted counts |
|---|---:|---:|---|---|
| 1 / 25% | 0.579 V | 179 | 0.13–0.22 | 133–225 |
| 2 / 50% | 1.052 V | 326 | 0.27–0.37 | 277–378 |
| 3 / 75% | 1.650 V | 512 | 0.45–0.55 | 461–562 |
| 4 / 100% | 2.245 V | 696 | 0.63–0.73 | 645–746 |

The mathematically checked 1% resistor corner ranges are contained within these windows. The windows leave unused gaps for fault/transition detection. They are starting values for bench validation, not a substitute for measuring noise and contact behavior with the actual harness.

## Firmware integration

The previous proportional-potentiometer interpretation must change. Configure the intended ADC resolution explicitly and normalize the reading by its maximum count. Map only readings inside the four accepted windows to positions 1–4, then to the requested 25%, 50%, 75% and 100%. At 10 bits use 1023; at 12 bits use 4095. Keep the speed and acceleration decoder states separate. Teensy analog inputs must stay within 0–3.3 V; see [PJRC Teensy 4.1](https://www.pjrc.com/store/teensy41.html).

The **N** switch is break-before-make, so it briefly opens between positions. The pull-up makes an open common or disconnected cable read near full scale, which is **invalid**, not 100%. A common-to-ground short reads near zero and is also invalid. Other wiring or resistor faults can resemble a valid selection; this is not complete fault detection.

A suitable starting decoder samples regularly, marks a changed or out-of-window input invalid, and accepts a new position only after it remains in the same valid window for at least **40 ms**. Initialize both inputs invalid at startup. Expose validity separately from percentage and never retain a stale setting indefinitely after disconnection. The controller must define the appropriate controlled-stop/inhibit response to invalid input; in particular, a zero acceleration setting must not accidentally disable braking. Actual motor-control behavior and debounce timing must be validated in the user's firmware.

No motor-control firmware was supplied or modified. These notes specify the hardware interface and suggested input decoding only.

## Switch contact choice and bench checks

Your exact **Q** part has silver contacts. The manufacturer's table on page 5 lists silver for power switching and B/G gold options for low-level/dry-circuit use. This input draws approximately **0.22–0.58 mA** through a selected contact. The datasheet does not establish Q-contact reliability at that current. The PCB is wired for your selected part, but its long-term contact performance is unverified; seek manufacturer confirmation or validate a suitable gold-contact equivalent for deployment if low-current operation cannot be established.

With power disconnected, confirm that A connects only to the selected terminal 1–4 and that terminal 9 is isolated. After assembly, measure each common voltage in all four detents with the final cable installed and verify correct software selection, transition handling, startup behavior and unplugged-input handling before enabling movement. This board has not undergone powered, environmental or system-level safety validation.
