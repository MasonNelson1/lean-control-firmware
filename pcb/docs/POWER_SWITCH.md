# Owned illuminated latching power switch

The user selected the owned Gebildet M12 NO SPST latching switch, advertised with a 12-24 V LED. It is separate from the four Dirvcp momentary Cal/mode buttons. The mechanical switch contacts and LED are separate circuits.

The current design is a **prototype interface requiring a bench check on the actual VESC 6 MkVI hardware and installed firmware**. No firmware/configuration on the user's VESCs has been read or changed. Do not assume that a latching switch will operate correctly with the default momentary-toggle configuration.

## Cable connections

J16 is a rear 4-pin JST-XH header; use an XHP-4 housing at the PCB end.

| J16 pin | Connection |
|---:|---|
| 1 | Switch dry contact A / ground |
| 2 | Switch dry contact B / shared VESC power-switch control |
| 3 | Owned power switch LED positive / direct 5 V |
| 4 | Owned power switch LED negative / ground |

Identify the two switch contacts using continuity in the two latched positions. Identify LED polarity from actual terminal markings or the switch's verified wiring diagram; **do not infer function from wire colors**. The intended state is closed-to-ground for ON and open for OFF. Confirm the LED current and usable brightness at a current-limited 5 V before connecting it to the panel.

J17 is the rear two-pin panel-to-main connector. Its straight-through cable to J23 remains 1=switch signal, 2=ground. All VESC cables start at the main PCB.

## Main-board VESC COMM connectors (revision M)

The former J24 two-pin header had two copies of the switch signal, one per VESC. It did not short the switch across ground. It is replaced by **two eight-pin B8B-PH-K-S headers with PHR-8 cable housings**, matching the physical COMM contact order shown in the user's MkVI pinout.

| Contact position, counted from the VESC 5V end | VESC label | J24 / primary | J25 / secondary |
|---:|---|---|---|
| 1 | 5V | NC | NC |
| 2 | VCC | NC | NC |
| 3 | GND | GND | NC |
| 4 | ADC1 | NC | NC |
| 5 | RX | NC | NC |
| 6 | TX | NC | NC |
| 7 | ADC2 | NC | NC |
| 8 | Power Switch | VESC_PWR_SW | VESC_PWR_SW |

Make one cable from J24 to the primary VESC COMM with only cavities **3 and 8** populated at both ends. Make a second from J25 to the secondary COMM with **only cavity 8** populated. Four PHR-8 housings and six PH contacts total. Count from the 5V end and confirm continuity end-to-end; do not rely on wire colors or apparent left/right when looking into opposite connector faces. All unused PCB pads are electrically isolated.

The button's dry contact connects VESC_PWR_SW to GND when closed. Its path is J16 -> J17 -> J23 -> J24.8/J25.8. Both output switch pins must share this net for one SPST switch to control both controllers; neither is permanently connected to GND. J24.3 provides the primary controller's return. Primary J2 already supplies 5V and GND; this added primary COMM return terminates at the same controller, not the second controller.

**Do not populate a GND lead to the secondary VESC.** The two VESCs must share their intended battery-negative connection. TRAMPA's switch diagram explicitly uses only the Power Switch lead on the second controller. Adding another secondary ground would create a return loop between controllers. No VESC supply output is connected through either COMM header.

Primary J2 remains 1=5V, 2=CANH, 3=CANL, 4=GND. Secondary J1 remains 1=NC, 2=CANH, 3=CANL, 4=NC. Keep their CAN cables as previously documented.

Connector order is checked against the user's normal MkVI image. The manufacturer product page links manual 251 as MkVI, but the PDF itself is titled **MkVI HP**; its contact drawing agrees with the user's image and its dual-switch topology is used as related-family evidence. TRAMPA's normal MkVI product page independently states support for a shared switch. This does not verify the installed controller firmware or the owned latching switch's operation.

## Direct 5 V indicator supply

At the user's request, J16.3 is connected to CP_5V, supplied through J14.2 from main J10.2. J16.4 is CP_GND. The owned switch is advertised for a 12-24 V LED, but will be operated at 5 V. Brightness and current have not been measured; it may be dim or fail to illuminate. This does not alter the separate dry-contact circuit. Test the LED with a current-limited 5 V bench supply before installation.

U102, its branch fuse F101 and input bulk capacitor C106 are removed. C107 remains a 100 nF bypass from CP_5V to CP_GND. No 12 V rail or boost converter remains. No fuse has been bridged in a retained boost circuit: that circuit is absent. The remaining 5 V feed has no new local PTC; source fault protection and cable-current limits have not been bench verified.

The panel does not carry battery or motor power and does not interrupt the battery. The switch requests VESC power-state control. Its LED shows the available panel supply, not an independently verified state of both controllers.

## VESC behavior to verify

The upstream VESC source has `SHUTDOWN_MODE_ALWAYS_OFF` behavior that can allow a physical switch to hold the regulator on and shut down when released. The VESC6 hardware code samples shutdown state using voltage decay rather than treating it as a simple MCU GPIO. **Using that mode for this latching switch is an inference from the source, not a verified configuration for the user's installed firmware.** The similarly named VESC6 HP reference is a related family document, not proof of every normal MkVI connector detail.

Before installing on the chair, test each controller independently with motors made unable to move: cold-start with switch open/closed; transition ON/OFF; behavior after removing/reapplying supply; then both controllers together. Confirm that neither controller remains enabled after the intended OFF command and that cable disconnection produces the intended safe state. Confirm actual port pins before making that connection. If the installed mode does not support the latching behavior, this interface needs revision or firmware configuration work before use.

Primary references checked during design:

- [TRAMPA VESC6 Mk6](https://trampa.co.uk/product/vesc6-mk6/)
- [VESC shutdown implementation](https://github.com/vedderb/bldc/blob/master/hwconf/shutdown.c)
- [VESC6 hardware implementation](https://github.com/vedderb/bldc/blob/master/hwconf/trampa/vesc6/hw_60_core.c) and [definitions](https://github.com/vedderb/bldc/blob/master/hwconf/trampa/vesc6/hw_60_core.h)
- [VESC Tool 7.00 app configuration definitions](https://github.com/vedderb/vesc_tool/blob/master/res/config/7.00/parameters_appconf.xml)

Downloaded source snapshots are retained in `references`; they are references, not the user's installed firmware. No claim of tested power-switch compatibility or wheelchair operational readiness is made by passing KiCad checks.

Additional wiring references: [TRAMPA linked switch diagram, page 4](https://trampaboards.com/resources/manuals/251.pdf), [manufacturer confirmation of JST PH 2 mm connector family](https://vesc-project.com/node/36). The downloaded PDF is retained as `references/TRAMPA-linked-MkVI-HP-manual-251.pdf`.
